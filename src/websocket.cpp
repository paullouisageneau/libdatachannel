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

#if RTC_ENABLE_WEBSOCKET

#include "include.hpp"
#include "websocket.hpp"

#include "tcptransport.hpp"
#include "tlstransport.hpp"
#include "wstransport.hpp"

#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rtc {

WebSocket::WebSocket() {}

WebSocket::WebSocket(const string &url) : WebSocket() { open(url); }

WebSocket::~WebSocket() { remoteClose(); }

WebSocket::State WebSocket::readyState() const { return mState; }

void WebSocket::open(const string &url) {
	if (mState != State::Closed)
		throw std::runtime_error("WebSocket must be closed before opening");

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

	changeState(State::Connecting);
	initTcpTransport();
}

void WebSocket::close() {
	auto state = mState.load();
	if (state == State::Connecting || state == State::Open) {
		changeState(State::Closing);
		if (auto transport = std::atomic_load(&mWsTransport))
			transport->close();
		else
			changeState(State::Closed);
	}
}

void WebSocket::remoteClose() {
	close();
	closeTransports();
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

bool WebSocket::isOpen() const { return mState == State::Open; }

bool WebSocket::isClosed() const { return mState == State::Closed; }

size_t WebSocket::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

std::optional<std::variant<binary, string>> WebSocket::receive() {
	while (!mRecvQueue.empty()) {
		auto message = *mRecvQueue.pop();
		switch (message->type) {
		case Message::String:
			return std::make_optional(
			    string(reinterpret_cast<const char *>(message->data()), message->size()));
		case Message::Binary:
			return std::make_optional(std::move(*message));
		default:
			// Ignore
			break;
		}
	}
	return nullopt;
}

size_t WebSocket::availableAmount() const { return mRecvQueue.amount(); }

bool WebSocket::changeState(State state) { return mState.exchange(state) != state; }

bool WebSocket::outgoing(message_ptr message) {
	if (mState != State::Open || !mWsTransport)
		throw std::runtime_error("WebSocket is not open");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

	return mWsTransport->send(message);
}

void WebSocket::incoming(message_ptr message) {
	if (message->type == Message::String || message->type == Message::Binary) {
		mRecvQueue.push(message);
		triggerAvailable(mRecvQueue.size());
	}
}

std::shared_ptr<TcpTransport> WebSocket::initTcpTransport() {
	using State = TcpTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mTcpTransport))
			return transport;

		auto transport = std::make_shared<TcpTransport>(
		    mHostname, mService, [this, weak_this = weak_from_this()](State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (state) {
			    case State::Connected:
				    if (mScheme == "ws")
					    initWsTransport();
				    else
					    initTlsTransport();
				    break;
			    case State::Failed:
				    triggerError("TCP connection failed");
				    remoteClose();
				    break;
			    case State::Disconnected:
				    remoteClose();
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });
		std::atomic_store(&mTcpTransport, transport);
		if (mState == WebSocket::State::Closed) {
			mTcpTransport.reset();
			transport->stop();
			throw std::runtime_error("Connection is closed");
		}
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
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
		auto transport = std::make_shared<TlsTransport>(
		    lower, mHost, [this, weak_this = weak_from_this()](State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (state) {
			    case State::Connected:
				    initWsTransport();
				    break;
			    case State::Failed:
				    triggerError("TCP connection failed");
				    remoteClose();
				    break;
			    case State::Disconnected:
				    remoteClose();
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });
		std::atomic_store(&mTlsTransport, transport);
		if (mState == WebSocket::State::Closed) {
			mTlsTransport.reset();
			transport->stop();
			throw std::runtime_error("Connection is closed");
		}
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
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
		auto transport = std::make_shared<WsTransport>(
		    lower, mHost, mPath, weak_bind(&WebSocket::incoming, this, _1),
		    [this, weak_this = weak_from_this()](State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (state) {
			    case State::Connected:
				    if (mState == WebSocket::State::Connecting) {
					    PLOG_DEBUG << "WebSocket open";
					    changeState(WebSocket::State::Open);
					    triggerOpen();
				    }
				    break;
			    case State::Failed:
				    triggerError("WebSocket connection failed");
				    remoteClose();
				    break;
			    case State::Disconnected:
				    remoteClose();
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });
		std::atomic_store(&mWsTransport, transport);
		if (mState == WebSocket::State::Closed) {
			mWsTransport.reset();
			transport->stop();
			throw std::runtime_error("Connection is closed");
		}
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("WebSocket transport initialization failed");
	}
}

void WebSocket::closeTransports() {
	changeState(State::Closed);

	// Pass the references to a thread, allowing to terminate a transport from its own thread
	auto ws = std::atomic_exchange(&mWsTransport, decltype(mWsTransport)(nullptr));
	auto tls = std::atomic_exchange(&mTlsTransport, decltype(mTlsTransport)(nullptr));
	auto tcp = std::atomic_exchange(&mTcpTransport, decltype(mTcpTransport)(nullptr));
	if (ws || tls || tcp) {
		std::thread t([ws, tls, tcp, token = mInitToken]() mutable {
			if (ws)
				ws->stop();
			if (tls)
				tls->stop();
			if (tcp)
				tcp->stop();

			ws.reset();
			tls.reset();
			tcp.reset();
		});
		t.detach();
	}
}

} // namespace rtc

#endif
