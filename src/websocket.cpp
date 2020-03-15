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

#include "include.hpp"
#include "websocket.hpp"

#include "tcptransport.hpp"
#include "tlstransport.hpp"
#include "wstransport.hpp"

#include <regex>

namespace rtc {

WebSocket::WebSocket() {}

WebSocket::WebSocket(const string &url) : WebSocket() { open(url); }

WebSocket::~WebSocket() { close(); }

void WebSocket::open(const string &url) {
	static const char *rs = R"(^(([^:\/?#]+):)?(//([^\/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?)";
	static std::regex regex(rs, std::regex::extended);

	std::smatch match;
	if (!std::regex_match(url, match, regex))
		throw std::invalid_argument("Malformed WebSocket URL: " + url);

	mScheme = match[2];
	if (mScheme != "ws" && mScheme != "wss")
		throw std::invalid_argument("Invalid WebSocket scheme: " + mScheme);

	mHost = match[4];
	if (auto pos = mHost.find(':'); pos != string::npos) {
		mHostname = mHost.substr(0, pos);
		mService = mHost.substr(pos + 1);
	} else {
		mHostname = mHost;
		mService = mScheme == "ws" ? "80" : "443";
	}

	mPath = match[5];
	if (string query = match[7]; !query.empty())
		mPath += "?" + query;

	initTcpTransport();
}

void WebSocket::close() {
	resetCallbacks();
	closeTransports();
}

void WebSocket::remoteClose() {
	mIsOpen = false;
	if (!mIsClosed.exchange(true))
		triggerClosed();
}

bool WebSocket::send(const std::variant<binary, string> &data) {
	return std::visit(
	    [&](const auto &d) {
		    using T = std::decay_t<decltype(d)>;
		    constexpr auto type = std::is_same_v<T, string> ? Message::String : Message::Binary;
		    auto *b = reinterpret_cast<const byte *>(d.data());
		    return outgoing(std::make_shared<Message>(b, b + d.size(), type));
	    },
	    data);
}

bool WebSocket::isOpen() const { return mIsOpen; }

bool WebSocket::isClosed() const { return mIsClosed; }

size_t WebSocket::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

std::optional<std::variant<binary, string>> WebSocket::receive() { return nullopt; }

size_t WebSocket::availableAmount() const { return 0; }

bool WebSocket::outgoing(mutable_message_ptr message) {
	if (mIsClosed || !mWsTransport)
		throw std::runtime_error("WebSocket is closed");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

	return mWsTransport->send(message);
}

std::shared_ptr<TcpTransport> WebSocket::initTcpTransport() {
	using State = TcpTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mTcpTransport))
			return transport;

		auto transport = std::make_shared<TcpTransport>(mHostname, mService, [this](State state) {
			switch (state) {
			case State::Connected:
				if (mScheme == "ws")
					initWsTransport();
				else
					initTlsTransport();
				break;
			case State::Failed:
				// TODO
				break;
			case State::Disconnected:
				// TODO
				break;
			default:
				// Ignore
				break;
			}
		});
		std::atomic_store(&mTcpTransport, transport);
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		// TODO
		throw std::runtime_error("TCP transport initialization failed");
	}
}

std::shared_ptr<TlsTransport> WebSocket::initTlsTransport() {
	using State = TlsTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mTlsTransport))
			return transport;

		auto lower = std::atomic_load(&mTcpTransport);
		auto transport = std::make_shared<TlsTransport>(lower, mHost, [this](State state) {
			switch (state) {
			case State::Connected:
				initWsTransport();
				break;
			case State::Failed:
				// TODO
				break;
			case State::Disconnected:
				// TODO
				break;
			default:
				// Ignore
				break;
			}
		});
		std::atomic_store(&mTlsTransport, transport);
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		// TODO
		throw std::runtime_error("TLS transport initialization failed");
	}
}

std::shared_ptr<WsTransport> WebSocket::initWsTransport() {
	using State = WsTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mWsTransport))
			return transport;

		std::shared_ptr<Transport> lower = std::atomic_load(&mTlsTransport);
		if (!lower)
			lower = std::atomic_load(&mTcpTransport);
		auto transport = std::make_shared<WsTransport>(lower, mHost, mPath, [this](State state) {
			switch (state) {
			case State::Connected:
				triggerOpen();
				break;
			case State::Failed:
				// TODO
				break;
			case State::Disconnected:
				// TODO
				break;
			default:
				// Ignore
				break;
			}
		});
		std::atomic_store(&mWsTransport, transport);
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		// TODO
		throw std::runtime_error("WebSocket transport initialization failed");
	}
}

void closeTransports() {
	mIsOpen = false;
	mIsClosed = true;

	// Pass the references to a thread, allowing to terminate a transport from its own thread
	auto ws = std::atomic_exchange(&mWsTransport, decltype(mWsTransport)(nullptr));
	auto dtls = std::atomic_exchange(&mDtlsTransport, decltype(mDtlsTransport)(nullptr));
	auto tcp = std::atomic_exchange(&mTcpTransport, decltype(mTcpTransport)(nullptr));
	if (ws || dtls || tcp) {
		std::thread t([ws, dtls, tcp]() mutable {
			if (ws)
				ws->stop();
			if (dtls)
				dtls->stop();
			if (tcp)
				tcp->stop();

			ws.reset();
			dtls.reset();
			tcp.reset();
		});
		t.detach();
	}
}

} // namespace rtc

#endif
