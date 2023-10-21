/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_WEBSOCKET

#include "websocket.hpp"
#include "common.hpp"
#include "internals.hpp"
#include "processor.hpp"
#include "utils.hpp"

#include "httpproxytransport.hpp"
#include "tcptransport.hpp"
#include "tlstransport.hpp"
#include "verifiedtlstransport.hpp"
#include "wstransport.hpp"

#include <array>
#include <chrono>
#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rtc::impl {

using namespace std::placeholders;
using namespace std::chrono_literals;
using std::chrono::milliseconds;

WebSocket::WebSocket(optional<Configuration> optConfig, certificate_ptr certificate)
    : config(optConfig ? std::move(*optConfig) : Configuration()),
      mCertificate(std::move(certificate)), mIsSecure(mCertificate != nullptr),
      mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {
	PLOG_VERBOSE << "Creating WebSocket";
	if (config.proxyServer) {
		if (config.proxyServer->type == ProxyServer::Type::Socks5)
			throw std::invalid_argument(
			    "Proxy server support for WebSocket is not implemented for Socks5");
		if (config.proxyServer->username || config.proxyServer->password) {
			PLOG_WARNING << "HTTP authentication support for proxy is not implemented";
		}
	}
}

WebSocket::~WebSocket() { PLOG_VERBOSE << "Destroying WebSocket"; }

void WebSocket::open(const string &url) {
	PLOG_VERBOSE << "Opening WebSocket to URL: " << url;

	if (state != State::Closed)
		throw std::logic_error("WebSocket must be closed before opening");

	// Modified regex from RFC 3986, see https://www.rfc-editor.org/rfc/rfc3986.html#appendix-B
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

	string username = utils::url_decode(m[6]);
	string password = utils::url_decode(m[8]);
	if (!username.empty() || !password.empty()) {
		PLOG_WARNING << "HTTP authentication support for WebSocket is not implemented";
	}

	string host;
	string hostname = m[10];
	string service = m[12];
	if (service.empty()) {
		service = mIsSecure ? "443" : "80";
		host = hostname;
	} else {
		host = hostname + ':' + service;
	}

	if (hostname.front() == '[' && hostname.back() == ']') {
		// IPv6 literal
		hostname.erase(hostname.begin());
		hostname.pop_back();
	} else {
		hostname = utils::url_decode(hostname);
	}

	string path = m[13];
	if (path.empty())
		path += '/';

	if (string query = m[15]; !query.empty())
		path += "?" + query;

	mHostname = hostname; // for TLS SNI and Proxy
	mService = service;   // For proxy
	std::atomic_store(&mWsHandshake, std::make_shared<WsHandshake>(host, path, config.protocols));

	changeState(State::Connecting);

	if (config.proxyServer) {
		setTcpTransport(std::make_shared<TcpTransport>(
		    config.proxyServer->hostname, std::to_string(config.proxyServer->port), nullptr));
	} else {
		setTcpTransport(std::make_shared<TcpTransport>(hostname, service, nullptr));
	}
}

void WebSocket::close() {
	auto s = state.load();
	if (s == State::Connecting || s == State::Open) {
		PLOG_VERBOSE << "Closing WebSocket";
		changeState(State::Closing);
		if (auto transport = std::atomic_load(&mWsTransport))
			transport->stop();
		else
			remoteClose();
	}
}

void WebSocket::remoteClose() {
	close();
	if (state.load() != State::Closed)
		closeTransports();
}

bool WebSocket::isOpen() const { return state == State::Open; }

bool WebSocket::isClosed() const { return state == State::Closed; }

size_t WebSocket::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

optional<message_variant> WebSocket::receive() {
	auto next = mRecvQueue.pop();
	return next ? std::make_optional(to_variant(std::move(**next))) : nullopt;
}

optional<message_variant> WebSocket::peek() {
	auto next = mRecvQueue.peek();
	return next ? std::make_optional(to_variant(std::move(**next))) : nullopt;
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
	std::atomic_store(member, transport);
	try {
		transport->start();
	} catch (...) {
		std::atomic_store(member, decltype(transport)(nullptr));
		transport->stop();
		throw;
	}

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

		transport->onBufferedAmount(weak_bind(&WebSocket::triggerBufferedAmount, this, _1));

		transport->onStateChange([this, weak_this = weak_from_this()](State transportState) {
			auto shared_this = weak_this.lock();
			if (!shared_this)
				return;
			switch (transportState) {
			case State::Connected:
				if (config.proxyServer)
					initProxyTransport();
				else if (mIsSecure)
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

		// WS transport sends a ping on read timeout
		auto pingInterval = config.pingInterval.value_or(10000ms);
		if (pingInterval > milliseconds::zero())
			transport->setReadTimeout(pingInterval);

		scheduleConnectionTimeout();

		return emplaceTransport(this, &mTcpTransport, std::move(transport));

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("TCP transport initialization failed");
	}
}

shared_ptr<HttpProxyTransport> WebSocket::initProxyTransport() {
	PLOG_VERBOSE << "Starting Tcp Proxy transport";
	using State = HttpProxyTransport::State;
	try {
		if (auto transport = std::atomic_load(&mProxyTransport))
			return transport;

		auto lower = std::atomic_load(&mTcpTransport);
		if (!lower)
			throw std::logic_error("No underlying TCP transport for Proxy transport");

		auto stateChangeCallback = [this, weak_this = weak_from_this()](State transportState) {
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
				triggerError("Proxy connection failed");
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

		auto transport = std::make_shared<HttpProxyTransport>(
		    lower, mHostname.value(), mService.value(), stateChangeCallback);

		return emplaceTransport(this, &mProxyTransport, std::move(transport));

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("Tcp Proxy transport initialization failed");
	}
}

shared_ptr<TlsTransport> WebSocket::initTlsTransport() {
	PLOG_VERBOSE << "Starting TLS transport";
	using State = TlsTransport::State;
	try {
		if (auto transport = std::atomic_load(&mTlsTransport))
			return transport;

		variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>> lower;
		if (config.proxyServer) {
			auto transport = std::atomic_load(&mProxyTransport);
			if (!transport)
				throw std::logic_error("No underlying proxy transport for TLS transport");

			lower = transport;
		} else {
			auto transport = std::atomic_load(&mTcpTransport);
			if (!transport)
				throw std::logic_error("No underlying TCP transport for TLS transport");

			lower = transport;
		}

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
			                                                   mCertificate, stateChangeCallback,
			                                                   config.caCertificatePemFile);
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

		variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>, shared_ptr<TlsTransport>>
		    lower;
		if (mIsSecure) {
			auto transport = std::atomic_load(&mTlsTransport);
			if (!transport)
				throw std::logic_error("No underlying TLS transport for WebSocket transport");

			lower = transport;
		} else if (config.proxyServer) {
			auto transport = std::atomic_load(&mProxyTransport);
			if (!transport)
				throw std::logic_error("No underlying proxy transport for WebSocket transport");

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

		auto maxOutstandingPings = config.maxOutstandingPings.value_or(0);
		auto transport = std::make_shared<WsTransport>(lower, mWsHandshake, maxOutstandingPings,
		                                               weak_bind(&WebSocket::incoming, this, _1),
		                                               stateChangeCallback);

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

	if (tcp)
		tcp->onBufferedAmount(nullptr);

	using array = std::array<shared_ptr<Transport>, 3>;
	array transports{std::move(ws), std::move(tls), std::move(tcp)};

	for (const auto &t : transports)
		if (t)
			t->onStateChange(nullptr);

	TearDownProcessor::Instance().enqueue(
	    [transports = std::move(transports), token = Init::Instance().token()]() mutable {
		    for (const auto &t : transports) {
			    if (t) {
				    t->stop();
				    break;
			    }
		    }

		    for (auto &t : transports)
			    t.reset();
	    });

	triggerClosed();
}

void WebSocket::scheduleConnectionTimeout() {
	auto defaultTimeout = 30s;
	auto timeout = config.connectionTimeout.value_or(milliseconds(defaultTimeout));
	if (timeout > milliseconds::zero()) {
		ThreadPool::Instance().schedule(timeout, [weak_this = weak_from_this()]() {
			if (auto locked = weak_this.lock()) {
				if (locked->state == WebSocket::State::Connecting) {
					PLOG_WARNING << "WebSocket connection timed out";
					locked->triggerError("Connection timed out");
					locked->remoteClose();
				}
			}
		});
	}
}

} // namespace rtc::impl

#endif
