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

#if RTC_ENABLE_WEBSOCKET

#include "websocketserver.hpp"
#include "common.hpp"

#include "impl/internals.hpp"
#include "impl/websocketserver.hpp"

namespace rtc {

WebSocketServer::WebSocketServer() : WebSocketServer(Configuration()) {}

WebSocketServer::WebSocketServer(Configuration config)
    : CheshireCat<impl::WebSocketServer>(std::move(config)) {}

WebSocketServer::~WebSocketServer() { impl()->stop(); }

void WebSocketServer::stop() { impl()->stop(); }

uint16_t WebSocketServer::port() const { return impl()->tcpServer->port(); }

void WebSocketServer::onClient(std::function<void(shared_ptr<WebSocket>)> callback) {
	impl()->clientCallback = callback;
}

} // namespace rtc

#endif
