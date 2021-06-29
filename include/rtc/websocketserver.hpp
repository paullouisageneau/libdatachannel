/**
 * Copyright (c) 2021 Paul-Louis Ageneau
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

#ifndef RTC_WEBSOCKETSERVER_H
#define RTC_WEBSOCKETSERVER_H

#if RTC_ENABLE_WEBSOCKET

#include "common.hpp"
#include "websocket.hpp"

namespace rtc {

namespace impl {

struct WebSocketServer;

}

class RTC_CPP_EXPORT WebSocketServer final : private CheshireCat<impl::WebSocketServer> {
public:
	struct Configuration {
		uint16_t port = 8080;
		bool enableTls = false;
		optional<string> certificatePemFile;
		optional<string> keyPemFile;
		optional<string> keyPemPass;
	};

	WebSocketServer();
	WebSocketServer(Configuration config);
	~WebSocketServer();

	void stop();

	uint16_t port() const;

	void onClient(std::function<void(shared_ptr<WebSocket>)> callback);

private:
	using CheshireCat<impl::WebSocketServer>::impl;
};

} // namespace rtc

#endif

#endif // RTC_WEBSOCKET_H
