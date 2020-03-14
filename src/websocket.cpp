/*************************************************************************
 *   Copyright (C) 2017-2018 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of Plateform.                                     *
 *                                                                       *
 *   Plateform is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   Plateform is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with Plateform.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#include "net/websocket.hpp"

#include <exception>
#include <iostream>

const size_t DEFAULT_MAX_PAYLOAD_SIZE = 16384; // 16 KB

namespace net {

WebSocket::WebSocket(void) : mMaxPayloadSize(DEFAULT_MAX_PAYLOAD_SIZE) {}

WebSocket::WebSocket(const string &url) : WebSocket() { open(url); }

WebSocket::~WebSocket(void) {}

void WebSocket::open(const string &url) {
	close();

	mUrl = url;
	mThread = std::thread(&WebSocket::run, this);
}

void WebSocket::close(void) {
	mWebSocket.close();
	if (mThread.joinable())
		mThread.join();
	mConnected = false;
}

bool WebSocket::isOpen(void) const { return mConnected; }

bool WebSocket::isClosed(void) const { return !mThread.joinable(); }

void WebSocket::setMaxPayloadSize(size_t size) { mMaxPayloadSize = size; }

bool WebSocket::send(const std::variant<binary, string> &data) {
	if (!std::holds_alternative<binary>(data))
		throw std::runtime_error("WebSocket string messages are not supported");

	mWebSocket.write(std::get<binary>(data));
	return true;
}

std::optional<std::variant<binary, string>> WebSocket::receive() {
	if (!mQueue.empty())
		return mQueue.pop();
	else
		return std::nullopt;
}

void WebSocket::run(void) {
	if (mUrl.empty())
		return;

	try {
		mWebSocket.connect(mUrl);

		mConnected = true;
		triggerOpen();

		while (true) {
			binary payload;
			if (!mWebSocket.read(payload, mMaxPayloadSize))
				break;
			mQueue.push(std::move(payload));
			triggerAvailable(mQueue.size());
		}
	} catch (const std::exception &e) {
		triggerError(e.what());
	}

	mWebSocket.close();

	if (mConnected)
		triggerClosed();
	mConnected = false;
}

} // namespace net
