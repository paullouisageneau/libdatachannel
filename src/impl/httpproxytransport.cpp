/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 * Copyright (c) 2023 Eric Gressman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "httpproxytransport.hpp"
#include "http.hpp"
#include "tcptransport.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

using std::to_string;
using std::chrono::system_clock;

HttpProxyTransport::HttpProxyTransport(shared_ptr<TcpTransport> lower, std::string hostname,
                                       std::string service, state_callback stateCallback)
    : Transport(lower, std::move(stateCallback)), mHostname(std::move(hostname)),
      mService(std::move(service)) {
	PLOG_DEBUG << "Initializing HTTP proxy transport";
	if (!lower->isActive())
		throw std::logic_error("HTTP proxy transport expects the lower transport to be active");
}

HttpProxyTransport::~HttpProxyTransport() { unregisterIncoming(); }

void HttpProxyTransport::start() {
	registerIncoming();

	changeState(State::Connecting);
	sendHttpRequest();
}

void HttpProxyTransport::stop() { unregisterIncoming(); }

bool HttpProxyTransport::send(message_ptr message) {
	if (state() != State::Connected)
		throw std::runtime_error("HTTP proxy connection is not open");

	PLOG_VERBOSE << "Send size=" << message->size();
	return outgoing(message);
}

bool HttpProxyTransport::isActive() const { return true; }

void HttpProxyTransport::incoming(message_ptr message) {
	auto s = state();
	if (s != State::Connecting && s != State::Connected)
		return; // Drop

	if (message) {
		PLOG_VERBOSE << "Incoming size=" << message->size();

		try {
			if (state() == State::Connecting) {
				mBuffer.insert(mBuffer.end(), message->begin(), message->end());
				if (size_t len = parseHttpResponse(mBuffer.data(), mBuffer.size())) {
					PLOG_INFO << "HTTP proxy connection open";
					changeState(State::Connected);
					mBuffer.erase(mBuffer.begin(), mBuffer.begin() + len);

					if (!mBuffer.empty()) {
						recv(make_message(mBuffer));
						mBuffer.clear();
					}
				}
			} else if (state() == State::Connected) {
				recv(std::move(message));
			}

			return;
		} catch (const std::exception &e) {
			PLOG_ERROR << e.what();
		}
	}

	if (state() == State::Connected) {
		PLOG_INFO << "HTTP proxy disconnected";
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		PLOG_ERROR << "HTTP proxy connection failed";
		changeState(State::Failed);
	}
}

bool HttpProxyTransport::sendHttpRequest() {
	PLOG_DEBUG << "Sending HTTP request to proxy";

	const string request = generateHttpRequest();
	auto data = reinterpret_cast<const byte *>(request.data());
	return outgoing(make_message(data, data + request.size()));
}

string HttpProxyTransport::generateHttpRequest() {
	return "CONNECT " + mHostname + ":" + mService + " HTTP/1.1\r\nHost: " + mHostname + "\r\n\r\n";
}

size_t HttpProxyTransport::parseHttpResponse(std::byte *buffer, size_t size) {
	std::list<string> lines;
	size_t length = parseHttpLines(buffer, size, lines);
	if (length == 0)
		return 0;

	if (lines.empty())
		throw std::runtime_error("Invalid response from HTTP proxy");

	std::istringstream status(std::move(lines.front()));
	lines.pop_front();

	string protocol;
	unsigned int code = 0;
	status >> protocol >> code;

	if (code != 200)
		throw std::runtime_error("Unexpected response code " + to_string(code) +
		                         " from HTTP proxy");

	return length;
}

} // namespace rtc::impl

#endif
