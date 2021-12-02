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

#if RTC_ENABLE_WEBSOCKET

#include "websocket.hpp"
#include "common.hpp"
#include "internals.hpp"
#include "threadpool.hpp"

#include "tcptransport.hpp"
#include "tlstransport.hpp"
#include "verifiedtlstransport.hpp"
#include "wstransport.hpp"

#include <array>
#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rtc::impl {

using namespace std::placeholders;

WebSocket::WebSocket(optional<Configuration> optConfig, certificate_ptr certificate)
    : config(optConfig ? std::move(*optConfig) : Configuration()),
      mCertificate(std::move(certificate)), mIsSecure(mCertificate != nullptr),
      mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {
	PLOG_VERBOSE << "Creating WebSocket";
}

WebSocket::~WebSocket() {
	PLOG_VERBOSE << "Destroying WebSocket";
	remoteClose();
}

void WebSocket::open(const string &url) {
	PLOG_VERBOSE << "Opening WebSocket to URL: " << url;

	if (state != State::Closed)
		throw std::logic_error("WebSocket must be closed before opening");

	// Modified regex from RFC 3986, see https://tools.ietf.org/html/rfc3986#appendix-B
	static const char *rs =
	    R"(^(([^:.@/?#]+):)?(/{0,2}((([^:@]*)(:([^@]*))?)@)?(([^:/?#]*)(:([^/?#]*))?))?([^?#]*)(\?([^#]*))?(#(.*))?)";

	static const std::regex r(rs, std::regex::extended);

	std::smatch m;
	if (!std::regex_match(url, m, r) || m[10].length() == 0)
		throw std::invalid_argument("Invalid WebSocket URL: " + url);

	string scheme = m[2];
	if (scheme.empty())
		scheme = "ws";

	if (scheme != "ws" && scheme != "wss")
		throw std::invalid_argument("Invalid WebSocket scheme: " + scheme);

	mIsSecure = (scheme != "ws");

	string host;
	string hostname = m[10];
	string service = m[12];
	if (service.empty()) {
		service = mIsSecure ? "443" : "80";
		host = hostname;
	} else {
		host = hostname + ':' + service;
	}

	while (!hostname.empty() && hostname.front() == '[')
		hostname.erase(hostname.begin());
	while (!hostname.empty() && hostname.back() == ']')
		hostname.pop_back();

	string path = m[13];
	if (path.empty())
		path += '/';

	if (string query = m[15]; !query.empty())
		path += "?" + query;

	mHostname = hostname; // for TLS SNI
	std::atomic_store(&mWsHandshake, std::make_shared<WsHandshake>(host, path, config.protocols));

	changeState(State::Connecting);
	setTcpTransport(std::make_shared<TcpTransport>(hostname, service, nullptr));
}

void WebSocket::close() {
	auto s = state.load();
	if (s == State::Connecting || s == State::Open) {
		PLOG_VERBOSE << "Closing WebSocket";
		changeState(State::Closing);
		if (auto transport = std::atomic_load(&mWsTransport))
			transport->close();
		else
			changeState(State::Closed);
	}
}

void WebSocket::remoteClose() {
	if (state != State::Closed) {
		close();
		closeTransports();
	}
}

bool WebSocket::isOpen() const { return state == State::Open; }

bool WebSocket::isClosed() const { return state == State::Closed; }

size_t WebSocket::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

optional<message_variant> WebSocket::receive() {
	while (auto next = mRecvQueue.tryPop()) {
		message_ptr message = *next;
		if (message->type != Message::Control)
			return to_variant(std::move(*message));
	}
	return nullopt;
}

optional<message_variant> WebSocket::peek() {
	while (auto next = mRecvQueue.peek()) {
		message_ptr message = *next;
		if (message->type != Message::Control)
			return to_variant(std::move(*message));

		mRecvQueue.tryPop();
	}
	return nullopt;
}

size_t WebSocket::availableAmount() const { return mRecvQueue.amount(); }

bool WebSocket::changeState(State newState) { return state.exchange(newState) != newState; }

bool WebSocket::outgoing(message_ptr message) {
	if (state != State::Open || !mWsTransport)
		throw std::runtime_error("WebSocket is not open");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

	return mWsTransport->send(message);
}

void WebSocket::incoming(message_ptr message) {
	if (!message) {
		remoteClose();
		return;
	}

	if (message->type == Message::String || message->type == Message::Binary) {
		mRecvQueue.push(message);
		triggerAvailable(mRecvQueue.size());
	}
}

// Helper for WebSocket::initXTransport methods: start and emplace the transport
template <typename T>
shared_ptr<T> emplaceTransport(WebSocket *ws, shared_ptr<T> *member, shared_ptr<T> transport) {
	transport->start();
	std::atomic_store(member, transport);
	if (ws->state == WebSocket::State::Closed) {
		std::atomic_store(member, decltype(transport)(nullptr));
		transport->stop();
		return nullptr;
	}
	return transport;
}

shared_ptr<TcpTransport> WebSocket::setTcpTransport(shared_ptr<TcpTransport> transport) {
	PLOG_VERBOSE << "Starting TCP transport";

	if (!transport)
		throw std::logic_error("TCP transport is null");

	using State = TcpTransport::State;
	try {
		if (std::atomic_load(&mTcpTransport))
			throw std::logic_error("TCP transport is already set");

		transport->onStateChange([this, weak_this = weak_from_this()](State transportState) {
			auto shared_this = weak_this.lock();
			if (!shared_this)
				return;
			switch (transportState) {
			case State::Connected:
				if (mIsSecure)
					initTlsTransport();
				else
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

		return emplaceTransport(this, &mTcpTransport, std::move(transport));

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("TCP transport initialization failed");
	}
}

shared_ptr<TlsTransport> WebSocket::initTlsTransport() {
	PLOG_VERBOSE << "Starting TLS transport";
	using State = TlsTransport::State;
	try {
		if (auto transport = std::atomic_load(&mTlsTransport))
			return transport;

		auto lower = std::atomic_load(&mTcpTransport);
		if (!lower)
			throw std::logic_error("No underlying TCP transport for TLS transport");

		auto stateChangeCallback = [this, weak_this = weak_from_this()](State transportState) {
			auto shared_this = weak_this.lock();
			if (!shared_this)
				return;
			switch (transportState) {
			case State::Connected:
				initWsTransport();
				break;
			case State::Failed:
				triggerError("TLS connection failed");
				remoteClose();
				break;
			case State::Disconnected:
				remoteClose();
				break;
			default:
				// Ignore
				break;
			}
		};

		bool verify = mHostname.has_value() && !config.disableTlsVerification;

#ifdef _WIN32
		if (std::exchange(verify, false)) {
			PLOG_WARNING << "TLS certificate verification with root CA is not supported on Windows";
		}
#endif

		shared_ptr<TlsTransport> transport;
		if (verify)
			transport = std::make_shared<VerifiedTlsTransport>(lower, mHostname.value(),
			                                                   mCertificate, stateChangeCallback);
		else
			transport =
			    std::make_shared<TlsTransport>(lower, mHostname, mCertificate, stateChangeCallback);

		return emplaceTransport(this, &mTlsTransport, std::move(transport));

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("TLS transport initialization failed");
	}
}

shared_ptr<WsTransport> WebSocket::initWsTransport() {
	PLOG_VERBOSE << "Starting WebSocket transport";
	using State = WsTransport::State;
	try {
		if (auto transport = std::atomic_load(&mWsTransport))
			return transport;

		variant<shared_ptr<TcpTransport>, shared_ptr<TlsTransport>> lower;
		if (mIsSecure) {
			auto transport = std::atomic_load(&mTlsTransport);
			if (!transport)
				throw std::logic_error("No underlying TLS transport for WebSocket transport");

			lower = transport;
		} else {
			auto transport = std::atomic_load(&mTcpTransport);
			if (!transport)
				throw std::logic_error("No underlying TCP transport for WebSocket transport");

			lower = transport;
		}

		if (!atomic_load(&mWsHandshake))
			atomic_store(&mWsHandshake, std::make_shared<WsHandshake>());

		auto stateChangeCallback = [this, weak_this = weak_from_this()](State transportState) {
			auto shared_this = weak_this.lock();
			if (!shared_this)
				return;
			switch (transportState) {
			case State::Connected:
				if (state == WebSocket::State::Connecting) {
					PLOG_DEBUG << "WebSocket open";
					if (changeState(WebSocket::State::Open))
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
		};

		auto transport = std::make_shared<WsTransport>(
		    lower, mWsHandshake, weak_bind(&WebSocket::incoming, this, _1), stateChangeCallback);

		return emplaceTransport(this, &mWsTransport, std::move(transport));

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("WebSocket transport initialization failed");
	}
}

shared_ptr<TcpTransport> WebSocket::getTcpTransport() const {
	return std::atomic_load(&mTcpTransport);
}

shared_ptr<TlsTransport> WebSocket::getTlsTransport() const {
	return std::atomic_load(&mTlsTransport);
}

shared_ptr<WsTransport> WebSocket::getWsTransport() const {
	return std::atomic_load(&mWsTransport);
}

shared_ptr<WsHandshake> WebSocket::getWsHandshake() const {
	return std::atomic_load(&mWsHandshake);
}

void WebSocket::closeTransports() {
	PLOG_VERBOSE << "Closing transports";

	if (!changeState(State::Closed))
		return; // already closed

	// Pass the pointers to a thread, allowing to terminate a transport from its own thread
	auto ws = std::atomic_exchange(&mWsTransport, decltype(mWsTransport)(nullptr));
	auto tls = std::atomic_exchange(&mTlsTransport, decltype(mTlsTransport)(nullptr));
	auto tcp = std::atomic_exchange(&mTcpTransport, decltype(mTcpTransport)(nullptr));

	if (ws)
		ws->onRecv(nullptr);

	using array = std::array<shared_ptr<Transport>, 3>;
	array transports{std::move(ws), std::move(tls), std::move(tcp)};

	for (const auto &t : transports)
		if (t)
			t->onStateChange(nullptr);

	ThreadPool::Instance().enqueue([transports = std::move(transports)]() mutable {
		for (const auto &t : transports)
			if (t)
				t->stop();

		for (auto &t : transports)
			t.reset();
	});

	triggerClosed();
}

} // namespace rtc::impl

#endif
