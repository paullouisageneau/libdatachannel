/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wstransport.hpp"
#include "httpproxytransport.hpp"
#include "tcptransport.hpp"
#include "threadpool.hpp"
#include "tlstransport.hpp"
#include "utils.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifndef htonll
#define htonll(x)                                                                                  \
	((uint64_t)(((uint64_t)htonl((uint32_t)(x))) << 32) | (uint64_t)htonl((uint32_t)((x) >> 32)))
#endif
#ifndef ntohll
#define ntohll(x) htonll(x)
#endif

namespace rtc::impl {

using std::to_integer;
using std::to_string;
using std::chrono::system_clock;

WsTransport::WsTransport(
    variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>, shared_ptr<TlsTransport>>
        lower,
    shared_ptr<WsHandshake> handshake, int maxOutstandingPings, message_callback recvCallback,
    state_callback stateCallback)
    : Transport(std::visit([](auto l) { return std::static_pointer_cast<Transport>(l); }, lower),
                std::move(stateCallback)),
      mHandshake(std::move(handshake)),
      mIsClient(
          std::visit(rtc::overloaded{[](auto l) { return l->isActive(); },
                                     [](shared_ptr<TlsTransport> l) { return l->isClient(); }},
                     lower)),
      mMaxOutstandingPings(maxOutstandingPings) {

	onRecv(std::move(recvCallback));

	PLOG_DEBUG << "Initializing WebSocket transport";
}

WsTransport::~WsTransport() { unregisterIncoming(); }

void WsTransport::start() {
	registerIncoming();

	changeState(State::Connecting);
	if (mIsClient)
		sendHttpRequest();
}

void WsTransport::stop() { close(); }

bool WsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();
	return sendFrame({message->type == Message::String ? TEXT_FRAME : BINARY_FRAME, message->data(),
	                  message->size(), true, mIsClient});
}

void WsTransport::close() {
	if (state() != State::Connected)
		return;

	if (mCloseSent.exchange(true))
		return;

	PLOG_INFO << "WebSocket closing";
	try {
		sendFrame({CLOSE, NULL, 0, true, mIsClient});
	} catch (const std::exception &e) {
		// The connection might not be open anymore
		PLOG_DEBUG << "Unable to send WebSocket close frame: " << e.what();
		changeState(State::Disconnected);
		return;
	}

	ThreadPool::Instance().schedule(std::chrono::seconds(10),
	                                [this, weak_this = weak_from_this()]() {
		                                if (auto shared_this = weak_this.lock()) {
			                                PLOG_DEBUG << "WebSocket close timeout";
			                                changeState(State::Disconnected);
		                                }
	                                });
}

void WsTransport::incoming(message_ptr message) {
	auto s = state();
	if (s != State::Connecting && s != State::Connected)
		return; // Drop

	if (message) {
		PLOG_VERBOSE << "Incoming size=" << message->size();

		try {
			mBuffer.insert(mBuffer.end(), message->begin(), message->end());

			if (state() == State::Connecting) {
				if (mIsClient) {
					if (size_t len =
					        mHandshake->parseHttpResponse(mBuffer.data(), mBuffer.size())) {
						PLOG_INFO << "WebSocket client-side open";
						changeState(State::Connected);
						mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
					}
				} else {
					if (size_t len = mHandshake->parseHttpRequest(mBuffer.data(), mBuffer.size())) {
						PLOG_INFO << "WebSocket server-side open";
						sendHttpResponse();
						changeState(State::Connected);
						mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
					}
				}
			}

			if (state() == State::Connected) {
				if (message->size() == 0) {
					// TCP is idle, send a ping
					PLOG_DEBUG << "WebSocket sending ping";
					uint32_t dummy = 0;
					sendFrame({PING, reinterpret_cast<byte *>(&dummy), 4, true, mIsClient});
					addOutstandingPing();
				} else {
					Frame frame;
					while (size_t len = readFrame(mBuffer.data(), mBuffer.size(), frame)) {
						recvFrame(frame);
						mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
					}
				}
			}

			return;

		} catch (const WsHandshake::RequestError &e) {
			PLOG_WARNING << e.what();
			try {
				sendHttpError(e.responseCode());

			} catch (const std::exception &e) {
				PLOG_WARNING << e.what();
			}

		} catch (const WsHandshake::Error &e) {
			PLOG_WARNING << e.what();

		} catch (const std::exception &e) {
			PLOG_ERROR << e.what();
		}
	}

	if (state() == State::Connected) {
		PLOG_INFO << "WebSocket disconnected";
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		PLOG_ERROR << "WebSocket handshake failed";
		changeState(State::Failed);
	}
}

bool WsTransport::sendHttpRequest() {
	PLOG_DEBUG << "Sending WebSocket HTTP request";

	const string request = mHandshake->generateHttpRequest();
	auto data = reinterpret_cast<const byte *>(request.data());
	return outgoing(make_message(data, data + request.size()));
}

bool WsTransport::sendHttpResponse() {
	PLOG_DEBUG << "Sending WebSocket HTTP response";

	const string response = mHandshake->generateHttpResponse();
	auto data = reinterpret_cast<const byte *>(response.data());
	return outgoing(make_message(data, data + response.size()));
}

bool WsTransport::sendHttpError(int code) {
	PLOG_WARNING << "Sending WebSocket HTTP error response " << code;

	const string response = mHandshake->generateHttpError(code);
	auto data = reinterpret_cast<const byte *>(response.data());
	return outgoing(make_message(data, data + response.size()));
}

// RFC6455 5.2. Base Framing Protocol
// https://www.rfc-editor.org/rfc/rfc6455.html#section-5.2
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-------+-+-------------+-------------------------------+
// |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
// |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
// |N|V|V|V|       |S|             |   (if payload len==126/127)   |
// | |1|2|3|       |K|             |                               |
// +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
// |    Extended payload length continued, if payload len == 127   |
// + - - - - - - - - - - - - - - - +-------------------------------+
// |                               | Masking-key, if MASK set to 1 |
// +-------------------------------+-------------------------------+
// |    Masking-key (continued)    |          Payload Data         |
// +-------------------------------+ - - - - - - - - - - - - - - - +
// :                     Payload Data continued ...                :
// + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
// |                     Payload Data continued ...                |
// +---------------------------------------------------------------+

size_t WsTransport::readFrame(byte *buffer, size_t size, Frame &frame) {
	const byte *end = buffer + size;
	if (end - buffer < 2)
		return 0;

	byte *cur = buffer;
	auto b1 = to_integer<uint8_t>(*cur++);
	auto b2 = to_integer<uint8_t>(*cur++);

	frame.fin = (b1 & 0x80) != 0;
	frame.mask = (b2 & 0x80) != 0;
	frame.opcode = static_cast<Opcode>(b1 & 0x0F);
	frame.length = b2 & 0x7F;

	if (frame.length == 0x7E) {
		if (end - cur < 2)
			return 0;
		frame.length = ntohs(*reinterpret_cast<const uint16_t *>(cur));
		cur += 2;
	} else if (frame.length == 0x7F) {
		if (end - cur < 8)
			return 0;
		frame.length = ntohll(*reinterpret_cast<const uint64_t *>(cur));
		cur += 8;
	}

	const byte *maskingKey = nullptr;
	if (frame.mask) {
		if (end - cur < 4)
			return 0;
		maskingKey = cur;
		cur += 4;
	}

	if (size_t(end - cur) < frame.length)
		return 0;

	frame.payload = cur;
	if (maskingKey)
		for (size_t i = 0; i < frame.length; ++i)
			frame.payload[i] ^= maskingKey[i % 4];
	cur += frame.length;

	return size_t(cur - buffer);
}

void WsTransport::recvFrame(const Frame &frame) {
	PLOG_DEBUG << "WebSocket received frame: opcode=" << int(frame.opcode)
	           << ", length=" << frame.length;

	switch (frame.opcode) {
	case TEXT_FRAME:
	case BINARY_FRAME: {
		if (!mPartial.empty()) {
			PLOG_WARNING << "WebSocket unfinished message: type="
			             << (mPartialOpcode == TEXT_FRAME ? "text" : "binary")
			             << ", length=" << mPartial.size();
			auto type = mPartialOpcode == TEXT_FRAME ? Message::String : Message::Binary;
			recv(make_message(mPartial.begin(), mPartial.end(), type));
			mPartial.clear();
		}
		mPartialOpcode = frame.opcode;
		if (frame.fin) {
			PLOG_DEBUG << "WebSocket finished message: type="
			           << (frame.opcode == TEXT_FRAME ? "text" : "binary")
			           << ", length=" << frame.length;
			auto type = frame.opcode == TEXT_FRAME ? Message::String : Message::Binary;
			recv(make_message(frame.payload, frame.payload + frame.length, type));
		} else {
			mPartial.insert(mPartial.end(), frame.payload, frame.payload + frame.length);
		}
		break;
	}
	case CONTINUATION: {
		mPartial.insert(mPartial.end(), frame.payload, frame.payload + frame.length);
		if (frame.fin) {
			PLOG_DEBUG << "WebSocket finished message: type="
			           << (frame.opcode == TEXT_FRAME ? "text" : "binary")
			           << ", length=" << mPartial.size();
			auto type = mPartialOpcode == TEXT_FRAME ? Message::String : Message::Binary;
			recv(make_message(mPartial.begin(), mPartial.end(), type));
			mPartial.clear();
		}
		break;
	}
	case PING: {
		PLOG_DEBUG << "WebSocket received ping, sending pong";
		sendFrame({PONG, frame.payload, frame.length, true, mIsClient});
		break;
	}
	case PONG: {
		PLOG_DEBUG << "WebSocket received pong";
		mOutstandingPings = 0;
		break;
	}
	case CLOSE: {
		PLOG_INFO << "WebSocket closed";
		close();
		changeState(State::Disconnected);
		break;
	}
	default: {
		PLOG_ERROR << "Unknown WebSocket opcode: " + to_string(frame.opcode);
		close();
		break;
	}
	}
}

bool WsTransport::sendFrame(const Frame &frame) {
	std::lock_guard lock(mSendMutex);

	PLOG_DEBUG << "WebSocket sending frame: opcode=" << int(frame.opcode)
	           << ", length=" << frame.length;

	byte buffer[14];
	byte *cur = buffer;

	*cur++ = byte((frame.opcode & 0x0F) | (frame.fin ? 0x80 : 0));

	if (frame.length < 0x7E) {
		*cur++ = byte((frame.length & 0x7F) | (frame.mask ? 0x80 : 0));
	} else if (frame.length <= 0xFFFF) {
		*cur++ = byte(0x7E | (frame.mask ? 0x80 : 0));
		*reinterpret_cast<uint16_t *>(cur) = htons(uint16_t(frame.length));
		cur += 2;
	} else {
		*cur++ = byte(0x7F | (frame.mask ? 0x80 : 0));
		*reinterpret_cast<uint64_t *>(cur) = htonll(uint64_t(frame.length));
		cur += 8;
	}

	if (frame.mask) {
		byte *maskingKey = reinterpret_cast<byte *>(cur);

		auto u = reinterpret_cast<uint8_t *>(maskingKey);
		std::generate(u, u + 4, utils::random_bytes_engine());
		cur += 4;

		for (size_t i = 0; i < frame.length; ++i)
			frame.payload[i] ^= maskingKey[i % 4];
	}

	const size_t length = cur - buffer; // header length
	auto message = make_message(length + frame.length);
	std::copy(buffer, buffer + length, message->begin()); // header
	std::copy(frame.payload, frame.payload + frame.length,
	          message->begin() + length); // payload

	return outgoing(std::move(message));
}

void WsTransport::addOutstandingPing() {
	++mOutstandingPings;
	if (mMaxOutstandingPings > 0 && mOutstandingPings > mMaxOutstandingPings) {
		changeState(State::Failed);
	}
}

} // namespace rtc::impl

#endif
