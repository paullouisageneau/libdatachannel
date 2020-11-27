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

#ifndef RTC_DATA_CHANNEL_H
#define RTC_DATA_CHANNEL_H

#include "channel.hpp"
#include "include.hpp"
#include "message.hpp"
#include "queue.hpp"
#include "reliability.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <type_traits>
#include <variant>

namespace rtc {

class SctpTransport;
class PeerConnection;

class RTC_CPP_EXPORT DataChannel : public std::enable_shared_from_this<DataChannel>, public Channel {
public:
	DataChannel(std::weak_ptr<PeerConnection> pc, uint16_t stream, string label, string protocol,
	            Reliability reliability);
	virtual ~DataChannel();

	uint16_t stream() const;
	uint16_t id() const;
	string label() const;
	string protocol() const;
	Reliability reliability() const;

	void close(void) override;
	bool send(message_variant data) override;
	bool send(const byte *data, size_t size) override;
	template <typename Buffer> bool sendBuffer(const Buffer &buf);
	template <typename Iterator> bool sendBuffer(Iterator first, Iterator last);

	bool isOpen(void) const override;
	bool isClosed(void) const override;
	size_t maxMessageSize() const override;

	// Extended API
	size_t availableAmount() const override;
	std::optional<message_variant> receive() override;
	std::optional<message_variant> peek() override;

protected:
	virtual void open(std::shared_ptr<SctpTransport> transport);
	virtual void processOpenMessage(message_ptr message);
	void remoteClose();
	bool outgoing(message_ptr message);
	void incoming(message_ptr message);

	const std::weak_ptr<PeerConnection> mPeerConnection;
	std::weak_ptr<SctpTransport> mSctpTransport;

	uint16_t mStream;
	string mLabel;
	string mProtocol;
	std::shared_ptr<Reliability> mReliability;

	std::atomic<bool> mIsOpen = false;
	std::atomic<bool> mIsClosed = false;

private:
	Queue<message_ptr> mRecvQueue;

	friend class PeerConnection;
};

class RTC_CPP_EXPORT NegociatedDataChannel final : public DataChannel {
public:
	NegociatedDataChannel(std::weak_ptr<PeerConnection> pc, uint16_t stream, string label,
	                      string protocol, Reliability reliability);
	NegociatedDataChannel(std::weak_ptr<PeerConnection> pc, std::weak_ptr<SctpTransport> transport,
	                      uint16_t stream);
	~NegociatedDataChannel();

private:
	void open(std::shared_ptr<SctpTransport> transport) override;
	void processOpenMessage(message_ptr message) override;

	friend class PeerConnection;
};

template <typename Buffer> std::pair<const byte *, size_t> to_bytes(const Buffer &buf) {
	using T = typename std::remove_pointer<decltype(buf.data())>::type;
	using E = typename std::conditional<std::is_void<T>::value, byte, T>::type;
	return std::make_pair(static_cast<const byte *>(static_cast<const void *>(buf.data())),
	                      buf.size() * sizeof(E));
}

template <typename Buffer> bool DataChannel::sendBuffer(const Buffer &buf) {
	auto [bytes, size] = to_bytes(buf);
	auto message = std::make_shared<Message>(size);
	std::copy(bytes, bytes + size, message->data());
	return outgoing(message);
}

template <typename Iterator> bool DataChannel::sendBuffer(Iterator first, Iterator last) {
	size_t size = 0;
	for (Iterator it = first; it != last; ++it)
		size += it->size();

	auto message = std::make_shared<Message>(size);
	auto pos = message->begin();
	for (Iterator it = first; it != last; ++it) {
		auto [bytes, len] = to_bytes(*it);
		pos = std::copy(bytes, bytes + len, pos);
	}
	return outgoing(message);
}

} // namespace rtc

#endif
