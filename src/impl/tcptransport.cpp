/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tcptransport.hpp"
#include "internals.hpp"
#include "threadpool.hpp"

#if RTC_ENABLE_WEBSOCKET

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <chrono>

namespace rtc::impl {

using namespace std::placeholders;
using namespace std::chrono_literals;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

TcpTransport::TcpTransport(string hostname, string service, state_callback callback)
    : Transport(nullptr, std::move(callback)), mIsActive(true), mHostname(std::move(hostname)),
      mService(std::move(service)), mSock(INVALID_SOCKET) {

	PLOG_DEBUG << "Initializing TCP transport";
}

TcpTransport::TcpTransport(socket_t sock, state_callback callback)
    : Transport(nullptr, std::move(callback)), mIsActive(false), mSock(sock) {

	PLOG_DEBUG << "Initializing TCP transport with socket";

	// Configure socket
	configureSocket();

	// Retrieve hostname and service
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	if (::getpeername(mSock, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) < 0)
		throw std::runtime_error("getsockname failed");

	char node[MAX_NUMERICNODE_LEN];
	char serv[MAX_NUMERICSERV_LEN];
	if (::getnameinfo(reinterpret_cast<struct sockaddr *>(&addr), addrlen, node,
	                  MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,
	                  NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		throw std::runtime_error("getnameinfo failed");

	mHostname = node;
	mService = serv;
}

TcpTransport::~TcpTransport() { close(); }

void TcpTransport::onBufferedAmount(amount_callback callback) {
	mBufferedAmountCallback = std::move(callback);
}

void TcpTransport::setReadTimeout(std::chrono::milliseconds readTimeout) {
	mReadTimeout = readTimeout;
}

void TcpTransport::start() {
	if (mSock == INVALID_SOCKET) {
		connect();
	} else {
		changeState(State::Connected);
		setPoll(PollService::Direction::In);
	}
}

bool TcpTransport::send(message_ptr message) {
	std::lock_guard lock(mSendMutex);

	if (state() != State::Connected)
		throw std::runtime_error("Connection is not open");

	if (!message || message->size() == 0)
		return trySendQueue();

	PLOG_VERBOSE << "Send size=" << message->size();
	return outgoing(message);
}

void TcpTransport::incoming(message_ptr message) {
	if (!message)
		return;

	PLOG_VERBOSE << "Incoming size=" << message->size();
	recv(message);
}

bool TcpTransport::outgoing(message_ptr message) {
	// mSendMutex must be locked
	// Flush the queue, and if nothing is pending, try to send directly
	if (trySendQueue() && trySendMessage(message))
		return true;

	mSendQueue.push(message);
	updateBufferedAmount(ptrdiff_t(message->size()));
	setPoll(PollService::Direction::Both);
	return false;
}

bool TcpTransport::isActive() const { return mIsActive; }

string TcpTransport::remoteAddress() const { return mHostname + ':' + mService; }

void TcpTransport::connect() {
	if (state() == State::Connecting)
		throw std::logic_error("TCP connection is already in progress");

	if (state() == State::Connected)
		throw std::logic_error("TCP is already connected");

	PLOG_DEBUG << "Connecting to " << mHostname << ":" << mService;
	changeState(State::Connecting);

	ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::resolve, this));
}

void TcpTransport::resolve() {
	std::lock_guard lock(mSendMutex);
	mResolved.clear();

	if (state() != State::Connecting)
		return; // Cancelled

	try {
		PLOG_DEBUG << "Resolving " << mHostname << ":" << mService;

		struct addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_ADDRCONFIG;

		struct addrinfo *result = nullptr;
		if (getaddrinfo(mHostname.c_str(), mService.c_str(), &hints, &result))
			throw std::runtime_error("Resolution failed for \"" + mHostname + ":" + mService +
			                         "\"");

		try {
			struct addrinfo *ai = result;
			while (ai) {
				struct sockaddr_storage addr;
				std::memcpy(&addr, ai->ai_addr, ai->ai_addrlen);
				mResolved.emplace_back(addr, socklen_t(ai->ai_addrlen));
				ai = ai->ai_next;
			}

		} catch (...) {
			freeaddrinfo(result);
			throw;
		}

		freeaddrinfo(result);

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		changeState(State::Failed);
		return;
	}

	ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::attempt, this));
}

void TcpTransport::attempt() {
	std::lock_guard lock(mSendMutex);

	if (state() != State::Connecting)
		return; // Cancelled

	if (mSock == INVALID_SOCKET) {
		::closesocket(mSock);
		mSock = INVALID_SOCKET;
	}

	if (mResolved.empty()) {
		PLOG_WARNING << "Connection to " << mHostname << ":" << mService << " failed";
		changeState(State::Failed);
		return;
	}

	try {
		auto [addr, addrlen] = mResolved.front();
		mResolved.pop_front();

		createSocket(reinterpret_cast<const struct sockaddr *>(&addr), addrlen);

	} catch (const std::runtime_error &e) {
		PLOG_DEBUG << e.what();
		ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::attempt, this));
		return;
	}

	// Poll out event callback
	auto callback = [this](PollService::Event event) {
		try {
			if (event == PollService::Event::Error)
				throw std::runtime_error("TCP connection failed");

			if (event == PollService::Event::Timeout)
				throw std::runtime_error("TCP connection timed out");

			if (event != PollService::Event::Out)
				return;

			int err = 0;
			socklen_t errlen = sizeof(err);
			if (::getsockopt(mSock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err),
			                 &errlen) != 0)
				throw std::runtime_error("Failed to get socket error code");

			if (err != 0) {
				std::ostringstream msg;
				msg << "TCP connection failed, errno=" << err;
				throw std::runtime_error(msg.str());
			}

			// Success
			PLOG_INFO << "TCP connected";
			changeState(State::Connected);
			setPoll(PollService::Direction::In);

		} catch (const std::exception &e) {
			PLOG_DEBUG << e.what();
			PollService::Instance().remove(mSock);
			ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::attempt, this));
		}
	};

	const auto timeout = 10s;
	PollService::Instance().add(mSock, {PollService::Direction::Out, timeout, std::move(callback)});
}

void TcpTransport::createSocket(const struct sockaddr *addr, socklen_t addrlen) {
	try {
		char node[MAX_NUMERICNODE_LEN];
		char serv[MAX_NUMERICSERV_LEN];
		if (getnameinfo(addr, addrlen, node, MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,
		                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
			PLOG_DEBUG << "Trying address " << node << ":" << serv;
		}

		PLOG_VERBOSE << "Creating TCP socket";

		// Create socket
		mSock = ::socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
		if (mSock == INVALID_SOCKET)
			throw std::runtime_error("TCP socket creation failed");

		// Configure socket
		configureSocket();

		// Initiate connection
		int ret = ::connect(mSock, addr, addrlen);
		if (ret < 0 && sockerrno != SEINPROGRESS && sockerrno != SEWOULDBLOCK) {
			std::ostringstream msg;
			msg << "TCP connection to " << node << ":" << serv << " failed, errno=" << sockerrno;
			throw std::runtime_error(msg.str());
		}

	} catch (...) {
		if (mSock != INVALID_SOCKET) {
			::closesocket(mSock);
			mSock = INVALID_SOCKET;
		}
		throw;
	}
}

void TcpTransport::configureSocket() {
	// Set non-blocking
	ctl_t nbio = 1;
	if (::ioctlsocket(mSock, FIONBIO, &nbio) < 0)
		throw std::runtime_error("Failed to set socket non-blocking mode");

	// Disable the Nagle algorithm
	int nodelay = 1;
	::setsockopt(mSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay),
	             sizeof(nodelay));

#ifdef __APPLE__
	// MacOS lacks MSG_NOSIGNAL and requires SO_NOSIGPIPE instead
	const sockopt_t enabled = 1;
	if (::setsockopt(mSock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
		throw std::runtime_error("Failed to disable SIGPIPE for socket");
#endif
}

void TcpTransport::setPoll(PollService::Direction direction) {
	PollService::Instance().add(
	    mSock, {direction, direction == PollService::Direction::In ? mReadTimeout : nullopt,
	            std::bind(&TcpTransport::process, this, _1)});
}

void TcpTransport::close() {
	std::lock_guard lock(mSendMutex);
	if (mSock != INVALID_SOCKET) {
		PLOG_DEBUG << "Closing TCP socket";
		PollService::Instance().remove(mSock);
		::closesocket(mSock);
		mSock = INVALID_SOCKET;
	}
	changeState(State::Disconnected);
}

bool TcpTransport::trySendQueue() {
	// mSendMutex must be locked
	while (auto next = mSendQueue.peek()) {
		message_ptr message = std::move(*next);
		size_t size = message->size();
		if (!trySendMessage(message)) { // replaces message
			mSendQueue.exchange(message);
			updateBufferedAmount(-ptrdiff_t(size) + ptrdiff_t(message->size()));
			return false;
		}

		mSendQueue.pop();
		updateBufferedAmount(-ptrdiff_t(size));
	}

	return true;
}

bool TcpTransport::trySendMessage(message_ptr &message) {
	// mSendMutex must be locked

	auto data = reinterpret_cast<const char *>(message->data());
	auto size = message->size();
	while (size) {
#if defined(__APPLE__) || defined(_WIN32)
		int flags = 0;
#else
		int flags = MSG_NOSIGNAL;
#endif
		int len = ::send(mSock, data, int(size), flags);
		if (len < 0) {
			if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK) {
				message = make_message(message->end() - size, message->end());
				return false;
			} else {
				PLOG_ERROR << "Connection closed, errno=" << sockerrno;
				throw std::runtime_error("Connection closed");
			}
		}

		data += len;
		size -= len;
	}
	message = nullptr;
	return true;
}

void TcpTransport::updateBufferedAmount(ptrdiff_t delta) {
	// Requires mSendMutex to be locked

	if (delta == 0)
		return;

	mBufferedAmount = size_t(std::max(ptrdiff_t(mBufferedAmount) + delta, ptrdiff_t(0)));

	// Synchronously call the buffered amount callback
	triggerBufferedAmount(mBufferedAmount);
}

void TcpTransport::triggerBufferedAmount(size_t amount) {
	try {
		mBufferedAmountCallback(amount);
	} catch (const std::exception &e) {
		PLOG_WARNING << "TCP buffered amount callback: " << e.what();
	}
}

void TcpTransport::process(PollService::Event event) {
	auto self = weak_from_this().lock();
	if (!self)
		return;

	try {
		switch (event) {
		case PollService::Event::Error: {
			PLOG_WARNING << "TCP connection terminated";
			break;
		}

		case PollService::Event::Timeout: {
			PLOG_VERBOSE << "TCP is idle";
			incoming(make_message(0));
			setPoll(PollService::Direction::In);
			return;
		}

		case PollService::Event::Out: {
			if (trySendQueue())
				setPoll(PollService::Direction::In);

			return;
		}

		case PollService::Event::In: {
			const size_t bufferSize = 4096;
			char buffer[bufferSize];
			int len;
			while ((len = ::recv(mSock, buffer, bufferSize, 0)) > 0) {
				auto *b = reinterpret_cast<byte *>(buffer);
				incoming(make_message(b, b + len));
			}

			if (len == 0)
				break; // clean close

			if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK) {
				PLOG_WARNING << "TCP connection lost";
				break;
			}

			return;
		}

		default:
			// Ignore
			return;
		}

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}

	PLOG_INFO << "TCP disconnected";
	PollService::Instance().remove(mSock);
	changeState(State::Disconnected);
	recv(nullptr);
}

} // namespace rtc::impl

#endif
