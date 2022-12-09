/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
