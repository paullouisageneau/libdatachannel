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

#if RTC_ENABLE_WEBSOCKET

#include "websocket.hpp"
#include "common.hpp"

#include "impl/websocket.hpp"
#include "impl/internals.hpp"

#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rtc {

using namespace std::placeholders;

WebSocket::WebSocket() : WebSocket(Configuration()) {}

WebSocket::WebSocket(Configuration config)
    : CheshireCat<impl::WebSocket>(std::move(config)),
      Channel(std::dynamic_pointer_cast<impl::Channel>(CheshireCat<impl::WebSocket>::impl())) {}

WebSocket::~WebSocket() { impl()->remoteClose(); }

WebSocket::State WebSocket::readyState() const { return impl()->state; }

bool WebSocket::isOpen() const { return impl()->state.load() == State::Open; }

bool WebSocket::isClosed() const { return impl()->state.load() == State::Closed; }

size_t WebSocket::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

void WebSocket::open(const string &url) {
	PLOG_VERBOSE << "Opening WebSocket to URL: " << url;

	impl()->parse(url);
	impl()->changeState(State::Connecting);
	impl()->initTcpTransport();
}

void WebSocket::close() {
	auto state = impl()->state.load();
	if (state == State::Connecting || state == State::Open) {
		PLOG_VERBOSE << "Closing WebSocket";
		impl()->changeState(State::Closing);
		if (auto transport = impl()->getWsTransport())
			transport->close();
		else
			impl()->changeState(State::Closed);
	}
}

bool WebSocket::send(message_variant data) {
	return impl()->outgoing(make_message(std::move(data)));
}

bool WebSocket::send(const byte *data, size_t size) {
	return impl()->outgoing(make_message(data, data + size));
}

} // namespace rtc

#endif
