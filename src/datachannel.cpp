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
#include "peerconnection.hpp"
#include "sctptransport.hpp"

namespace rtc {

using std::shared_ptr;

enum MessageType : uint8_t {
	MESSAGE_OPEN_REQUEST = 0x00,
	MESSAGE_OPEN_RESPONSE = 0x01,
	MESSAGE_ACK = 0x02,
	MESSAGE_OPEN = 0x03,
	MESSAGE_CLOSE = 0x04
};

struct OpenMessage {
	uint8_t type = MESSAGE_OPEN;
	uint8_t channelType;
	uint16_t priority;
	uint32_t reliabilityParameter;
	uint16_t labelLength;
	uint16_t protocolLength;
	// label
	// protocol
};

struct AckMessage {
	uint8_t type = MESSAGE_ACK;
};

struct CloseMessage {
	uint8_t type = MESSAGE_CLOSE;
};

DataChannel::DataChannel(shared_ptr<SctpTransport> sctpTransport, unsigned int streamId)
    : mSctpTransport(sctpTransport), mStreamId(streamId) {}

DataChannel::DataChannel(shared_ptr<SctpTransport> sctpTransport, unsigned int streamId,
                         string label, string protocol, Reliability reliability)
    : DataChannel(sctpTransport, streamId) {
	mLabel = std::move(label);
	mProtocol = std::move(protocol);
	mReliability = std::make_shared<Reliability>(std::move(reliability));
}

DataChannel::~DataChannel() { close(); }

void DataChannel::close() {
	mIsOpen = false;
	if (!mIsClosed) {
		mIsClosed = true;
		mSctpTransport->reset(mStreamId);
	}
}

void DataChannel::send(const std::variant<binary, string> &data) {
	std::visit(
	    [this](const auto &d) {
		    using T = std::decay_t<decltype(d)>;
		    constexpr auto type = std::is_same_v<T, string> ? Message::String : Message::Binary;
		    auto *b = reinterpret_cast<const byte *>(d.data());
		    mSctpTransport->send(make_message(b, b + d.size(), type, mStreamId, mReliability));
	    },
	    data);
}

void DataChannel::send(const byte *data, size_t size) {
	mSctpTransport->send(make_message(data, data + size, Message::Binary, mStreamId));
}

string DataChannel::label() const { return mLabel; }

string DataChannel::protocol() const { return mProtocol; }

Reliability DataChannel::reliability() const { return *mReliability; }

bool DataChannel::isOpen(void) const { return mIsOpen; }

bool DataChannel::isClosed(void) const { return mIsClosed; }

void DataChannel::open() {
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
	open.labelLength = htons(mLabel.size());
	open.protocolLength = htons(mProtocol.size());

	auto end = reinterpret_cast<char *>(buffer.data() + sizeof(OpenMessage));
	std::copy(mLabel.begin(), mLabel.end(), end);
	std::copy(mProtocol.begin(), mProtocol.end(), end + mLabel.size());

	mSctpTransport->send(make_message(buffer.begin(), buffer.end(), Message::Control, mStreamId));
}

void DataChannel::incoming(message_ptr message) {
	switch (message->type) {
	case Message::Control: {
		auto raw = reinterpret_cast<const uint8_t *>(message->data());
		switch (raw[0]) {
		case MESSAGE_OPEN:
			processOpenMessage(message);
			break;
		case MESSAGE_ACK:
			if (!mIsOpen) {
				mIsOpen = true;
				triggerOpen();
			}
			break;
		case MESSAGE_CLOSE:
			if (mIsOpen) {
				close();
				triggerClosed();
			}
			break;
		default:
			// Ignore
			break;
		}
		break;
	}
	case Message::String: {
		triggerMessage(string(reinterpret_cast<const char *>(message->data()), message->size()));
		break;
	}
	case Message::Binary: {
		triggerMessage(*message);
		break;
	}
	}
}

void DataChannel::processOpenMessage(message_ptr message) {
	auto *raw = reinterpret_cast<const uint8_t *>(message->data());

	if (message->size() < 12)
		throw std::invalid_argument("DataChannel open message too small");

	OpenMessage open;
	open.channelType = raw[1];
	open.priority = (raw[2] << 8) + raw[3];
	open.reliabilityParameter = (raw[4] << 24) + (raw[5] << 16) + (raw[6] << 8) + raw[7];
	open.labelLength = (raw[8] << 8) + raw[9];
	open.protocolLength = (raw[10] << 8) + raw[11];

	if (message->size() < 12 + open.labelLength + open.protocolLength)
		throw std::invalid_argument("DataChannel open message truncated");

	mLabel.assign(reinterpret_cast<const char *>(raw + 12), open.labelLength);
	mProtocol.assign(reinterpret_cast<const char *>(raw + 12 + open.labelLength),
	                 open.protocolLength);

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

	mSctpTransport->send(make_message(buffer.begin(), buffer.end(), Message::Control, mStreamId));

	triggerOpen();
}

} // namespace rtc
