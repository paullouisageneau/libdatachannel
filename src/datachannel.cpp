/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "datachannel.hpp"
#include "include.hpp"
#include "peerconnection.hpp"
#include "sctptransport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

using std::shared_ptr;
using std::weak_ptr;

// Messages for the DataChannel establishment protocol
// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09

enum MessageType : uint8_t {
	MESSAGE_OPEN_REQUEST = 0x00,
	MESSAGE_OPEN_RESPONSE = 0x01,
	MESSAGE_ACK = 0x02,
	MESSAGE_OPEN = 0x03,
	MESSAGE_CLOSE = 0x04
};

#pragma pack(push, 1)
struct OpenMessage {
	uint8_t type = MESSAGE_OPEN;
	uint8_t channelType;
	uint16_t priority;
	uint32_t reliabilityParameter;
	uint16_t labelLength;
	uint16_t protocolLength;
	// The following fields are:
	// uint8_t[labelLength] label
	// uint8_t[protocolLength] protocol
};

struct AckMessage {
	uint8_t type = MESSAGE_ACK;
};

struct CloseMessage {
	uint8_t type = MESSAGE_CLOSE;
};
#pragma pack(pop)

const size_t RECV_QUEUE_LIMIT = 1024 * 1024; // 1 MiB

DataChannel::DataChannel(weak_ptr<PeerConnection> pc, unsigned int stream, string label,
                         string protocol, Reliability reliability)
    : mPeerConnection(pc), mStream(stream), mLabel(std::move(label)),
      mProtocol(std::move(protocol)),
      mReliability(std::make_shared<Reliability>(std::move(reliability))),
      mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {}

DataChannel::DataChannel(weak_ptr<PeerConnection> pc, weak_ptr<SctpTransport> transport,
                         unsigned int stream)
    : mPeerConnection(pc), mSctpTransport(transport), mStream(stream),
      mReliability(std::make_shared<Reliability>()),
      mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {}

DataChannel::~DataChannel() {
	close();
}

unsigned int DataChannel::stream() const { return mStream; }

string DataChannel::label() const { return mLabel; }

string DataChannel::protocol() const { return mProtocol; }

Reliability DataChannel::reliability() const { return *mReliability; }

void DataChannel::close() {
	mIsClosed = true;
	if (mIsOpen.exchange(false))
		if (auto transport = mSctpTransport.lock())
			transport->closeStream(mStream);

	mSctpTransport.reset();
	resetCallbacks();
}

void DataChannel::remoteClose() {
	if (!mIsClosed.exchange(true))
		triggerClosed();

	mIsOpen = false;
	mSctpTransport.reset();
}

bool DataChannel::send(const std::variant<binary, string> &data) {
	return std::visit(
	    [&](const auto &d) {
		    using T = std::decay_t<decltype(d)>;
		    constexpr auto type = std::is_same_v<T, string> ? Message::String : Message::Binary;
		    auto *b = reinterpret_cast<const byte *>(d.data());
		    return outgoing(std::make_shared<Message>(b, b + d.size(), type));
	    },
	    data);
}

bool DataChannel::send(const byte *data, size_t size) {
	return outgoing(std::make_shared<Message>(data, data + size, Message::Binary));
}

std::optional<std::variant<binary, string>> DataChannel::receive() {
	while (!mRecvQueue.empty()) {
		auto message = *mRecvQueue.pop();
		switch (message->type) {
		case Message::Control: {
			auto raw = reinterpret_cast<const uint8_t *>(message->data());
			if (raw[0] == MESSAGE_CLOSE)
				remoteClose();
			break;
		}
		case Message::String:
			return std::make_optional(
			    string(reinterpret_cast<const char *>(message->data()), message->size()));
		case Message::Binary:
			return std::make_optional(std::move(*message));
		default:
			// Ignore
			break;
		}
	}

	return nullopt;
}

bool DataChannel::isOpen(void) const { return mIsOpen; }

bool DataChannel::isClosed(void) const { return mIsClosed; }

size_t DataChannel::maxMessageSize() const {
	size_t remoteMax = DEFAULT_MAX_MESSAGE_SIZE;
	if (auto pc = mPeerConnection.lock())
		if (auto description = pc->remoteDescription())
			if (auto maxMessageSize = description->maxMessageSize())
				remoteMax = *maxMessageSize > 0 ? *maxMessageSize : LOCAL_MAX_MESSAGE_SIZE;

	return std::min(remoteMax, LOCAL_MAX_MESSAGE_SIZE);
}

size_t DataChannel::availableAmount() const { return mRecvQueue.amount(); }

void DataChannel::open(shared_ptr<SctpTransport> transport) {
	mSctpTransport = transport;

	uint8_t channelType = static_cast<uint8_t>(mReliability->type);
	if (mReliability->unordered)
		channelType &= 0x80;

	using std::chrono::milliseconds;
	uint32_t reliabilityParameter = 0;
	if (mReliability->type == Reliability::TYPE_PARTIAL_RELIABLE_REXMIT)
		reliabilityParameter = uint32_t(std::get<int>(mReliability->rexmit));
	else if (mReliability->type == Reliability::TYPE_PARTIAL_RELIABLE_TIMED)
		reliabilityParameter = uint32_t(std::get<milliseconds>(mReliability->rexmit).count());

	const size_t len = sizeof(OpenMessage) + mLabel.size() + mProtocol.size();
	binary buffer(len, byte(0));
	auto &open = *reinterpret_cast<OpenMessage *>(buffer.data());
	open.type = MESSAGE_OPEN;
	open.channelType = mReliability->type;
	open.priority = htons(0);
	open.reliabilityParameter = htonl(reliabilityParameter);
	open.labelLength = htons(uint16_t(mLabel.size()));
	open.protocolLength = htons(uint16_t(mProtocol.size()));

	auto end = reinterpret_cast<char *>(buffer.data() + sizeof(OpenMessage));
	std::copy(mLabel.begin(), mLabel.end(), end);
	std::copy(mProtocol.begin(), mProtocol.end(), end + mLabel.size());

	transport->send(make_message(buffer.begin(), buffer.end(), Message::Control, mStream));
}

bool DataChannel::outgoing(message_ptr message) {
	if (mIsClosed)
		throw std::runtime_error("DataChannel is closed");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

	auto transport = mSctpTransport.lock();
	if (!transport)
		throw std::runtime_error("DataChannel has no transport");

	// Before the ACK has been received on a DataChannel, all messages must be sent ordered
	message->reliability = mIsOpen ? mReliability : nullptr;
	message->stream = mStream;
	return transport->send(message);
}

void DataChannel::incoming(message_ptr message) {
	if (!message)
		return;

	switch (message->type) {
	case Message::Control: {
		auto raw = reinterpret_cast<const uint8_t *>(message->data());
		switch (raw[0]) {
		case MESSAGE_OPEN:
			processOpenMessage(message);
			break;
		case MESSAGE_ACK:
			if (!mIsOpen.exchange(true)) {
				triggerOpen();
			}
			break;
		case MESSAGE_CLOSE:
			// The close message will be processed in-order in receive()
			mRecvQueue.push(message);
			triggerAvailable(mRecvQueue.size());
			break;
		default:
			// Ignore
			break;
		}
		break;
	}
	case Message::String:
	case Message::Binary:
		mRecvQueue.push(message);
		triggerAvailable(mRecvQueue.size());
		break;
	default:
		// Ignore
		break;
	}
}

void DataChannel::processOpenMessage(message_ptr message) {
	auto transport = mSctpTransport.lock();
	if (!transport)
		throw std::runtime_error("DataChannel has no transport");

	if (message->size() < sizeof(OpenMessage))
		throw std::invalid_argument("DataChannel open message too small");

	OpenMessage open = *reinterpret_cast<const OpenMessage *>(message->data());
	open.priority = ntohs(open.priority);
	open.reliabilityParameter = ntohl(open.reliabilityParameter);
	open.labelLength = ntohs(open.labelLength);
	open.protocolLength = ntohs(open.protocolLength);

	if (message->size() < sizeof(OpenMessage) + size_t(open.labelLength + open.protocolLength))
		throw std::invalid_argument("DataChannel open message truncated");

	auto end = reinterpret_cast<const char *>(message->data() + sizeof(OpenMessage));
	mLabel.assign(end, open.labelLength);
	mProtocol.assign(end + open.labelLength, open.protocolLength);

	using std::chrono::milliseconds;
	mReliability->unordered = (open.reliabilityParameter & 0x80) != 0;
	switch (open.channelType & 0x7F) {
	case Reliability::TYPE_PARTIAL_RELIABLE_REXMIT:
		mReliability->type = Reliability::TYPE_PARTIAL_RELIABLE_REXMIT;
		mReliability->rexmit = int(open.reliabilityParameter);
		break;
	case Reliability::TYPE_PARTIAL_RELIABLE_TIMED:
		mReliability->type = Reliability::TYPE_PARTIAL_RELIABLE_TIMED;
		mReliability->rexmit = milliseconds(open.reliabilityParameter);
		break;
	default:
		mReliability->type = Reliability::TYPE_RELIABLE;
		mReliability->rexmit = int(0);
	}

	binary buffer(sizeof(AckMessage), byte(0));
	auto &ack = *reinterpret_cast<AckMessage *>(buffer.data());
	ack.type = MESSAGE_ACK;

	transport->send(make_message(buffer.begin(), buffer.end(), Message::Control, mStream));

	mIsOpen = true;
	triggerOpen();
}

} // namespace rtc
