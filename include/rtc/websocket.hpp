/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_WEBSOCKET_H
#define RTC_WEBSOCKET_H

#if RTC_ENABLE_WEBSOCKET

#include "channel.hpp"
#include "common.hpp"
#include "configuration.hpp"

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

	using Configuration = WebSocketConfiguration;

	WebSocket();
	WebSocket(Configuration config);
	WebSocket(impl_ptr<impl::WebSocket> impl);
	~WebSocket() override;

	State readyState() const;

	bool isOpen() const override;
	bool isClosed() const override;
	size_t maxMessageSize() const override;

	void open(const string &url, const std::map<string, string> &headers = {});
	void close() override;
	void forceClose();
	bool send(const message_variant data) override;
	bool send(const byte *data, size_t size) override;

	optional<string> remoteAddress() const;
	optional<string> path() const;
	std::multimap<string, string> requestHeaders() const;

private:
	using CheshireCat<impl::WebSocket>::impl;
};

std::ostream &operator<<(std::ostream &out, WebSocket::State state);

} // namespace rtc

#endif

#endif // RTC_WEBSOCKET_H
