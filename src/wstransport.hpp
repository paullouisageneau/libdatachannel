/**
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#ifndef RTC_WS_TRANSPORT_H
#define RTC_WS_TRANSPORT_H

#if RTC_ENABLE_WEBSOCKET

#include "include.hpp"
#include "transport.hpp"

namespace rtc {

class TcpTransport;
class TlsTransport;

class WsTransport : public Transport {
public:
	WsTransport(std::shared_ptr<Transport> lower, string host, string path,
	            message_callback recvCallback, state_callback stateCallback);
	~WsTransport();

	bool stop() override;
	bool send(message_ptr message) override;
	bool send(mutable_message_ptr message);

	void incoming(message_ptr message) override;

	void close();

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
	size_t readHttpResponse(const byte *buffer, size_t size);

	size_t readFrame(byte *buffer, size_t size, Frame &frame);
	void recvFrame(const Frame &frame);
	bool sendFrame(const Frame &frame);

	const string mHost;
	const string mPath;

	binary mBuffer;
	binary mPartial;
	Opcode mPartialOpcode;
};

} // namespace rtc

#endif

#endif
