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
#include "common.hpp"
#include "message.hpp"
#include "reliability.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <shared_mutex>
#include <type_traits>
#include <shared_mutex>

namespace rtc {

namespace impl {

struct DataChannel;
struct PeerConnection;

} // namespace impl

class RTC_CPP_EXPORT DataChannel final : private CheshireCat<impl::DataChannel>, public Channel {
public:
	DataChannel(impl_ptr<impl::DataChannel> impl);
	virtual ~DataChannel();

	uint16_t stream() const;
	uint16_t id() const;
	string label() const;
	string protocol() const;
	Reliability reliability() const;

	bool isOpen(void) const override;
	bool isClosed(void) const override;
	size_t maxMessageSize() const override;

	void close(void) override;
	bool send(message_variant data) override;
	bool send(const byte *data, size_t size) override;
	template <typename Buffer> bool sendBuffer(const Buffer &buf);
	template <typename Iterator> bool sendBuffer(Iterator first, Iterator last);

private:
	using CheshireCat<impl::DataChannel>::impl;
};

template <typename Buffer> std::pair<const byte *, size_t> to_bytes(const Buffer &buf) {
	using T = typename std::remove_pointer<decltype(buf.data())>::type;
	using E = typename std::conditional<std::is_void<T>::value, byte, T>::type;
	return std::make_pair(static_cast<const byte *>(static_cast<const void *>(buf.data())),
	                      buf.size() * sizeof(E));
}

template <typename Buffer> bool DataChannel::sendBuffer(const Buffer &buf) {
	auto [bytes, size] = to_bytes(buf);
	return send(bytes, size);
}

template <typename Iterator> bool DataChannel::sendBuffer(Iterator first, Iterator last) {
	size_t size = 0;
	for (Iterator it = first; it != last; ++it)
		size += it->size();

	binary buffer(size);
	byte *pos = buffer.data();
	for (Iterator it = first; it != last; ++it) {
		auto [bytes, len] = to_bytes(*it);
		pos = std::copy(bytes, bytes + len, pos);
	}
	return send(std::move(buffer));
}

} // namespace rtc

#endif
