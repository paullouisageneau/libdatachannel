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

#include "wstransport.hpp"
#include "base64.hpp"
#include "tcptransport.hpp"
#include "tlstransport.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <chrono>
#include <iterator>
#include <list>
#include <map>
#include <numeric>
#include <random>
#include <regex>

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

using namespace std::chrono;
using std::to_integer;
using std::to_string;

using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned short>;

WsTransport::WsTransport(shared_ptr<Transport> lower, Configuration config,
                         message_callback recvCallback, state_callback stateCallback)
    : Transport(lower, std::move(stateCallback)), mConfig(std::move(config)) {
	onRecv(recvCallback);

	PLOG_DEBUG << "Initializing WebSocket transport";

	if (mConfig.host.empty())
		throw std::invalid_argument("WebSocket HTTP host cannot be empty");

	if (mConfig.path.empty())
		throw std::invalid_argument("WebSocket HTTP path cannot be empty");
}

WsTransport::~WsTransport() { stop(); }

void WsTransport::start() {
	Transport::start();

	registerIncoming();
	sendHttpRequest();
}

bool WsTransport::stop() {
	if (!Transport::stop())
		return false;

	close();
	return true;
}

bool WsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();
	return sendFrame({message->type == Message::String ? TEXT_FRAME : BINARY_FRAME, message->data(),
	                  message->size(), true, true});
}

void WsTransport::incoming(message_ptr message) {
	auto s = state();
	if (s != State::Connecting && s != State::Connected)
		return; // Drop

	if (message) {
		PLOG_VERBOSE << "Incoming size=" << message->size();

		if (message->size() == 0) {
			// TCP is idle, send a ping
			PLOG_DEBUG << "WebSocket sending ping";
			uint32_t dummy = 0;
			sendFrame({PING, reinterpret_cast<byte *>(&dummy), 4, true, true});
			return;
		}

		mBuffer.insert(mBuffer.end(), message->begin(), message->end());

		try {
			if (state() == State::Connecting) {
				if (size_t len = readHttpResponse(mBuffer.data(), mBuffer.size())) {
					PLOG_INFO << "WebSocket open";
					changeState(State::Connected);
					mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
				}
			}

			if (state() == State::Connected) {
				Frame frame;
				while (size_t len = readFrame(mBuffer.data(), mBuffer.size(), frame)) {
					recvFrame(frame);
					mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);
				}
			}

			return;

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

void WsTransport::close() {
	if (state() == State::Connected) {
		sendFrame({CLOSE, NULL, 0, true, true});
		PLOG_INFO << "WebSocket closing";
		changeState(State::Disconnected);
	}
}

bool WsTransport::sendHttpRequest() {
	PLOG_DEBUG << "Sending WebSocket HTTP request for path " << mConfig.path;
	changeState(State::Connecting);

	auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
	random_bytes_engine generator(seed);

	binary key(16);
	auto k = reinterpret_cast<uint8_t *>(key.data());
	std::generate(k, k + key.size(), [&]() { return uint8_t(generator()); });

	string appendHeader = "";
	if (mConfig.protocols.size() > 0) {
		appendHeader +=
		    "Sec-WebSocket-Protocol: " +
		    std::accumulate(mConfig.protocols.begin(), mConfig.protocols.end(), string(),
		                    [](const string &a, const string &b) -> string {
			                    return a + (a.length() > 0 ? "," : "") + b;
		                    }) +
		    "\r\n";
	}

	const string request = "GET " + mConfig.path +
	                       " HTTP/1.1\r\n"
	                       "Host: " +
	                       mConfig.host +
	                       "\r\n"
	                       "Connection: Upgrade\r\n"
	                       "Upgrade: websocket\r\n"
	                       "Sec-WebSocket-Version: 13\r\n"
	                       "Sec-WebSocket-Key: " +
	                       to_base64(key) + "\r\n" + std::move(appendHeader) + "\r\n";

	auto data = reinterpret_cast<const byte *>(request.data());
	auto size = request.size();
	return outgoing(make_message(data, data + size));
}

size_t WsTransport::readHttpResponse(const byte *buffer, size_t size) {
	std::list<string> lines;
	auto begin = reinterpret_cast<const char *>(buffer);
	auto end = begin + size;
	auto cur = begin;

	while (true) {
		auto last = cur;
		cur = std::find(cur, end, '\n');
		if (cur == end)
			return 0;
		string line(last, cur != begin && *std::prev(cur) == '\r' ? std::prev(cur++) : cur++);
		if (line.empty())
			break;
		lines.emplace_back(std::move(line));
	}
	size_t length = cur - begin;

	if (lines.empty())
		throw std::runtime_error("Invalid HTTP response for WebSocket");

	string status = std::move(lines.front());
	lines.pop_front();

	std::istringstream ss(status);
	string protocol;
	unsigned int code = 0;
	ss >> protocol >> code;
	PLOG_DEBUG << "WebSocket response code: " << code;
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
	if (h == headers.end())
		throw std::runtime_error("WebSocket update header missing");

	string upgrade;
	std::transform(h->second.begin(), h->second.end(), std::back_inserter(upgrade),
	               [](char c) { return std::tolower(c); });
	if (upgrade != "websocket")
		throw std::runtime_error("WebSocket update header mismatching: " + h->second);

	h = headers.find("sec-websocket-accept");
	if (h == headers.end())
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
		sendFrame({PONG, frame.payload, frame.length, true, true});
		break;
	}
	case PONG: {
		PLOG_DEBUG << "WebSocket received pong";
		break;
	}
	case CLOSE: {
		close();
		PLOG_INFO << "WebSocket closed";
		changeState(State::Disconnected);
		break;
	}
	default: {
		close();
		throw std::invalid_argument("Unknown WebSocket opcode: " + to_string(frame.opcode));
	}
	}
}

bool WsTransport::sendFrame(const Frame &frame) {
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
		auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
		random_bytes_engine generator(seed);

		byte *maskingKey = reinterpret_cast<byte *>(cur);

		auto u = reinterpret_cast<uint8_t *>(maskingKey);
		std::generate(u, u + 4, [&]() { return uint8_t(generator()); });
		cur += 4;

		for (size_t i = 0; i < frame.length; ++i)
			frame.payload[i] ^= maskingKey[i % 4];
	}

	outgoing(make_message(buffer, cur));                                        // header
	return outgoing(make_message(frame.payload, frame.payload + frame.length)); // payload
}

} // namespace rtc

#endif
