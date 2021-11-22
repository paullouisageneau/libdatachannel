/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
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

#ifndef RTC_WEBSOCKET_H
#define RTC_WEBSOCKET_H

#if RTC_ENABLE_WEBSOCKET

#include "channel.hpp"
#include "common.hpp"

namespace rtc {

namespace impl {

struct WebSocket;

}

class RTC_CPP_EXPORT WebSocket final : private CheshireCat<impl::WebSocket>, public Channel {
public:
	enum class State : int {
		Connecting = 0,
		Open = 1,
		Closing = 2,
		Closed = 3,
	};

	struct Configuration {
		bool disableTlsVerification = false; // if true, don't verify the TLS certificate
		std::vector<string> protocols;
	};

	WebSocket();
	WebSocket(Configuration config);
	WebSocket(impl_ptr<impl::WebSocket> impl);
	~WebSocket();

	State readyState() const;

	bool isOpen() const override;
	bool isClosed() const override;
	size_t maxMessageSize() const override;

	void open(const string &url);
	void close() override;
	bool send(const message_variant data) override;
	bool send(const byte *data, size_t size) override;

	optional<string> remoteAddress() const;
	optional<string> path() const;

private:
	using CheshireCat<impl::WebSocket>::impl;
};

} // namespace rtc

#endif

#endif // RTC_WEBSOCKET_H
