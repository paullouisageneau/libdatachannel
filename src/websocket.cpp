/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_WEBSOCKET

#include "websocket.hpp"
#include "common.hpp"

#include "impl/internals.hpp"
#include "impl/websocket.hpp"

namespace rtc {

WebSocket::WebSocket() : WebSocket(Configuration()) {}

WebSocket::WebSocket(Configuration config)
    : CheshireCat<impl::WebSocket>(std::move(config)),
      Channel(std::dynamic_pointer_cast<impl::Channel>(CheshireCat<impl::WebSocket>::impl())) {}

WebSocket::WebSocket(impl_ptr<impl::WebSocket> impl)
    : CheshireCat<impl::WebSocket>(std::move(impl)),
      Channel(std::dynamic_pointer_cast<impl::Channel>(CheshireCat<impl::WebSocket>::impl())) {}

WebSocket::~WebSocket() {
	try {
		impl()->remoteClose();
		impl()->resetCallbacks(); // not done by impl::WebSocket
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}
}

WebSocket::State WebSocket::readyState() const { return impl()->state; }

bool WebSocket::isOpen() const { return impl()->state.load() == State::Open; }

bool WebSocket::isClosed() const { return impl()->state.load() == State::Closed; }

size_t WebSocket::maxMessageSize() const { return impl()->maxMessageSize(); }

void WebSocket::open(const string &url) { impl()->open(url); }

void WebSocket::close() { impl()->close(); }

void WebSocket::forceClose() { impl()->remoteClose(); }

bool WebSocket::send(message_variant data) {
	return impl()->outgoing(make_message(std::move(data)));
}

bool WebSocket::send(const byte *data, size_t size) {
	return impl()->outgoing(make_message(data, data + size, Message::Binary));
}

optional<string> WebSocket::remoteAddress() const {
	auto tcpTransport = impl()->getTcpTransport();
	return tcpTransport ? make_optional(tcpTransport->remoteAddress()) : nullopt;
}

optional<string> WebSocket::path() const {
	auto state = impl()->state.load();
	auto handshake = impl()->getWsHandshake();
	return state != State::Connecting && handshake ? make_optional(handshake->path()) : nullopt;
}

std::ostream &operator<<(std::ostream &out, WebSocket::State state) {
	using State = WebSocket::State;
	const char *str;
	switch (state) {
	case State::Connecting:
		str = "connecting";
		break;
	case State::Open:
		str = "open";
		break;
	case State::Closing:
		str = "closing";
		break;
	case State::Closed:
		str = "closed";
		break;
	default:
		str = "unknown";
		break;
	}
	return out << str;
}

} // namespace rtc

#endif
