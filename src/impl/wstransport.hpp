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

#ifndef RTC_IMPL_WS_TRANSPORT_H
#define RTC_IMPL_WS_TRANSPORT_H

#include "common.hpp"
#include "transport.hpp"
#include "wshandshake.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

class TcpTransport;
class TlsTransport;

class WsTransport final : public Transport {
public:
	WsTransport(variant<shared_ptr<TcpTransport>, shared_ptr<TlsTransport>> lower,
	            shared_ptr<WsHandshake> handshake, message_callback recvCallback,
	            state_callback stateCallback);
	~WsTransport();

	void start() override;
	bool stop() override;
	bool send(message_ptr message) override;
	void incoming(message_ptr message) override;
	void close();

	bool isClient() const { return mIsClient; }

private:
	enum Opcode : uint8_t {
		CONTINUATION = 0,
		TEXT_FRAME = 1,
		BINARY_FRAME = 2,
		CLOSE = 8,
		PING = 9,
		PONG = 10,
	};

	struct Frame {
		Opcode opcode = BINARY_FRAME;
		byte *payload = nullptr;
		size_t length = 0;
		bool fin = true;
		bool mask = true;
	};

	bool sendHttpRequest();
	bool sendHttpError(int code);
	bool sendHttpResponse();

	size_t readFrame(byte *buffer, size_t size, Frame &frame);
	void recvFrame(const Frame &frame);
	bool sendFrame(const Frame &frame);

	const shared_ptr<WsHandshake> mHandshake;
	const bool mIsClient;

	binary mBuffer;
	binary mPartial;
	Opcode mPartialOpcode;
};

} // namespace rtc::impl

#endif

#endif
