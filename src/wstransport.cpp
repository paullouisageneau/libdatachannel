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

#if ENABLE_WEBSOCKET

#include "wstransport.hpp"
#include "tcptransport.hpp"
#include "tlstransport.hpp"

#include "base64.hpp"

#include <chrono>
#include <list>
#include <map>
#include <random>
#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifndef htonll
#define htonll(x)                                                                                  \
	((uint64_t)htonl(((uint64_t)(x)&0xFFFFFFFF) << 32) | (uint64_t)htonl((uint64_t)(x) >> 32))
#endif
#ifndef ntohll
#define ntohll(x) htonll(x)
#endif

namespace rtc {

using namespace std::chrono;
using std::to_integer;
using std::to_string;

using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned char>;

WsTransport::WsTransport(std::shared_ptr<TcpTransport> lower, string host, string path)
    : Transport(lower), mHost(std::move(host)), mPath(std::move(path)) {}

WsTransport::WsTransport(std::shared_ptr<TlsTransport> lower, string host, string path)
    : Transport(lower), mHost(std::move(host)), mPath(std::move(path)) {}

WsTransport::~WsTransport() {}

void WsTransport::stop() {}

bool WsTransport::send(message_ptr message) {
	if (!message)
		return false;

	// Call the mutable message overload with a copy
	return send(std::make_shared<Message>(*message));
}

bool WsTransport::send(mutable_message_ptr message) {
	if (!message)
		return false;

	return sendFrame({message->type == Message::String ? TEXT_FRAME : BINARY_FRAME, message->data(),
	                  message->size(), true, true});
}

void WsTransport::incoming(message_ptr message) {
	mBuffer.insert(mBuffer.end(), message->begin(), message->end());

	if (!mHandshakeDone) {
		if (size_t len = readHttpResponse(mBuffer.data(), mBuffer.size())) {
			mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
			mHandshakeDone = true;
		}
	}

	if (mHandshakeDone) {
		Frame frame = {};
		while (size_t len = readFrame(mBuffer.data(), mBuffer.size(), frame)) {
			mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
			recvFrame(frame);
		}
	}
}

void WsTransport::connect() { sendHttpRequest(); }

void WsTransport::close() {
	if (mHandshakeDone)
		sendFrame({CLOSE, NULL, 0, true, true});
}

bool WsTransport::sendHttpRequest() {
	auto seed = system_clock::now().time_since_epoch().count();
	random_bytes_engine generator(seed);

	binary key(16);
	std::generate(reinterpret_cast<uint8_t *>(key.data()),
	              reinterpret_cast<uint8_t *>(key.data() + key.size()), generator);

	const string request = "GET " + mPath +
	                       " HTTP/1.1\r\n"
	                       "Host: " +
	                       mHost +
	                       "\r\n"
	                       "Connection: Upgrade\r\n"
	                       "Upgrade: websocket\r\n"
	                       "Sec-WebSocket-Version: 13\r\n"
	                       "Sec-WebSocket-Key: " +
	                       to_base64(key) +
	                       "\r\n"
	                       "\r\n";

	auto data = reinterpret_cast<const byte *>(request.data());
	auto size = request.size();
	return outgoing(make_message(data, data + size));
}

size_t WsTransport::readHttpResponse(const byte *buffer, size_t size) {

	std::list<string> lines;
	auto begin = reinterpret_cast<const char *>(buffer);
	auto end = begin + size;
	auto cur = begin;
	while ((cur = std::find(cur, end, '\n')) != end) {
		string line(begin, cur != begin && *std::prev(cur) == '\r' ? std::prev(cur++) : cur++);
		if (line.empty())
			break;
		lines.emplace_back(std::move(line));
	}
	size_t length = cur - begin;

	string status = std::move(lines.front());
	lines.pop_front();

	std::istringstream ss(status);
	string protocol;
	unsigned int code = 0;
	ss >> protocol >> code;
	if (code != 101)
		throw std::runtime_error("Unexpected response code for WebSocket: " + to_string(code));

	std::multimap<string, string> headers;
	for (const auto &line : lines) {
		if (size_t pos = line.find_first_of(':'); pos != string::npos) {
			string key = line.substr(0, pos);
			string value = line.substr(line.find_first_not_of(' ', pos + 1));
			std::transform(key.begin(), key.end(), key.begin(),
			               [](char c) { return std::tolower(c); });
			headers.emplace(std::move(key), std::move(value));
		} else {
			headers.emplace(line, "");
		}
	}

	auto h = headers.find("upgrade");
	if (h == headers.end() || h->second != "websocket")
		throw std::runtime_error("WebSocket update header missing or mismatching");

	h = headers.find("sec-websocket-accept");
	throw std::runtime_error("WebSocket accept header missing");

	// TODO: Verify Sec-WebSocket-Accept

	return length;
}

// http://tools.ietf.org/html/rfc6455#section-5.2  Base Framing Protocol
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
			return false;
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

	if (end - cur < frame.length)
		return false;

	frame.payload = cur;
	if (maskingKey)
		for (size_t i = 0; i < frame.length; ++i)
			frame.payload[i] ^= maskingKey[i % 4];

	return end - buffer;
}

void WsTransport::recvFrame(const Frame &frame) {
	switch (frame.opcode) {
	case TEXT_FRAME:
	case BINARY_FRAME: {
		if (!mPartial.empty()) {
			auto type = mPartialOpcode == TEXT_FRAME ? Message::String : Message::Binary;
			recv(make_message(mPartial.begin(), mPartial.end(), type));
			mPartial.clear();
		}
		if (frame.fin) {
			auto type = frame.opcode == TEXT_FRAME ? Message::String : Message::Binary;
			recv(make_message(frame.payload, frame.payload + frame.length));
		} else {
			mPartial.insert(mPartial.end(), frame.payload, frame.payload + frame.length);
			mPartialOpcode = frame.opcode;
		}
		break;
	}
	case CONTINUATION: {
		mPartial.insert(mPartial.end(), frame.payload, frame.payload + frame.length);
		if (frame.fin) {
			auto type = mPartialOpcode == TEXT_FRAME ? Message::String : Message::Binary;
			recv(make_message(mPartial.begin(), mPartial.end()));
			mPartial.clear();
		}
		break;
	}
	case PING: {
		sendFrame({PONG, frame.payload, frame.length, true, true});
		break;
	}
	case PONG: {
		// TODO
		break;
	}
	case CLOSE: {
		close();
		break;
	}
	default: {
		close();
		throw std::invalid_argument("Unknown WebSocket opcode: " + to_string(frame.opcode));
	}
	}
}

bool WsTransport::sendFrame(const Frame &frame) {
	byte buffer[14];
	byte *cur = buffer;

	*cur++ = byte((frame.opcode & 0x0F) | (frame.fin ? 0x80 : 0));

	if (frame.length < 0x7E) {
		*cur++ = byte((frame.length & 0x7F) | (frame.mask ? 0x80 : 0));
	} else if (frame.length <= 0xFF) {
		*cur++ = byte(0x7E | (frame.mask ? 0x80 : 0));
		*reinterpret_cast<uint16_t *>(cur) = uint16_t(frame.length);
		cur += 2;
	} else {
		*cur++ = byte(0x7F | (frame.mask ? 0x80 : 0));
		*reinterpret_cast<uint64_t *>(cur) = uint64_t(frame.length);
		cur += 8;
	}

	if (frame.mask) {
		auto seed = system_clock::now().time_since_epoch().count();
		random_bytes_engine generator(seed);

		auto *maskingKey = cur;
		std::generate(reinterpret_cast<uint8_t *>(maskingKey),
		              reinterpret_cast<uint8_t *>(maskingKey + 4), generator);
		cur += 4;

		for (size_t i = 0; i < frame.length; ++i)
			frame.payload[i] ^= maskingKey[i % 4];
	}

	outgoing(make_message(buffer, cur));                                        // header
	return outgoing(make_message(frame.payload, frame.payload + frame.length)); // payload
}

} // namespace rtc

#endif
