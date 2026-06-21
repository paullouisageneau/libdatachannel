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

namespace {

bool unmap_inet6_v4mapped(struct sockaddr *sa, socklen_t *len) {
	if (sa->sa_family != AF_INET6)
		return false;

	const auto *sin6 = reinterpret_cast<struct sockaddr_in6 *>(sa);
	if (!IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
		return false;

	struct sockaddr_in6 copy = *sin6;
	sin6 = &copy;

	auto *sin = reinterpret_cast<struct sockaddr_in *>(sa);
	std::memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = sin6->sin6_port;
	std::memcpy(&sin->sin_addr, reinterpret_cast<const uint8_t *>(&sin6->sin6_addr) + 12, 4);
	*len = sizeof(*sin);
	return true;
}

} // namespace

TcpTransport::TcpTransport(string hostname, string service, state_callback callback)
    : Transport(nullptr, std::move(callback)), mIsActive(true), mHostname(std::move(hostname)),
      mService(std::move(service)), mSock(INVALID_SOCKET) {

	PLOG_DEBUG << "Initializing TCP transport";
}

TcpTransport::TcpTransport(socket_t sock, state_callback callback)
    : Transport(nullptr, std::move(callback)), mIsActive(false), mSock(sock) {

	PLOG_DEBUG << "Initializing TCP transport with socket";

	// Configure socket
	configureSocket(mSock);

	// Retrieve hostname and service
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	if (::getpeername(mSock, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) < 0)
		throw std::runtime_error("getpeername failed");

	unmap_inet6_v4mapped(reinterpret_cast<struct sockaddr *>(&addr), &addrlen);

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

void TcpTransport::setConnectAttemptDelay(std::chrono::milliseconds connectAttemptDelay) {
	mConnectAttemptDelay = connectAttemptDelay;
}

void TcpTransport::setConnectTimeout(std::chrono::milliseconds connectTimeout) {
	mConnectTimeout = connectTimeout;
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

// Reorders the list of addresses to alternate IPv4 and IPv6 addresses towards the beginning of the
// list whenever possible, while always keeping the first address in its original place.
static void interleave_address_families(addrinfo *&head) {
	for (addrinfo *current = head; current;) {
		int target = current->ai_family == AF_INET ? AF_INET6 : AF_INET;

		addrinfo **candidate_ptr = &current->ai_next;
		while (*candidate_ptr && (*candidate_ptr)->ai_family != target)
			candidate_ptr = &(*candidate_ptr)->ai_next;
		if (!*candidate_ptr)
			break;

		addrinfo *node = *candidate_ptr;
		*candidate_ptr = node->ai_next;

		node->ai_next = current->ai_next;
		current->ai_next = node;

		current = node;
	}
}

void TcpTransport::resolve() {
	std::lock_guard lock(mSendMutex);
	mResolved.clear();
	mPendingSocks = 0;

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

		// Interleave address families, so that an address of the opposite family of the first
		// listed address is attempted as early as possible. See section 4. Sorting Addresses of
		// RFC8305: https://www.rfc-editor.org/info/rfc8305/#section-4
		interleave_address_families(result);

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

	// FIXME Cache and reuse the address that we successfully connected to before

	socket_t sock = INVALID_SOCKET;
	bool isLastAttempt = false;

	try {
		std::lock_guard lock(mSendMutex);

		if (state() != State::Connecting)
			return; // Cancelled

		// Handle the rare case where the poll service emits two events in quick succession, while
		// concurrent connection attempts are enabled: A delayed connection attempt timeout event
		// that triggers another connection attempt asynchronously on the thread pool, followed by
		// an out event for a successful connection. The latter acquires the lock and closes all
		// sockets, then releases the lock before it changes the state to connected (see the
		// processConnect() method for reasoning). If this happens before the socket for the queued
		// attempt is created here, which is likely because the thread pool takes some time to
		// execute the next attempt, we would create a new socket while we have already established
		// a connection with a socket (race condition). This check prevents the creation of another
		// connection in this case, since access to mSock is synchronized via mSendMutex.
		if (mSock != INVALID_SOCKET)
			return;

		if (mResolved.empty()) {
			PLOG_WARNING << "Connection to " << mHostname << ":" << mService << " failed";
			closeUnusedSockets();
			changeState(State::Failed);
			return;
		}

		auto [addr, addrlen] = mResolved.front();
		mResolved.pop_front();
		isLastAttempt = mResolved.empty();

		sock = createSocket(reinterpret_cast<const struct sockaddr *>(&addr), addrlen);

		// We need to make sure that we only close the remaining sockets once all addresses have
		// been attempted or one of the addresses has successfully connected, as createSocket()
		// might otherwise return a file descriptor that we already encountered before, since it can
		// reuse closed file descriptors, and thereby cause issues with our use of a set here
		assert(mSocks.find(sock) == mSocks.end());

		mSocks.insert(sock);
		mPendingSocks += 1;

	} catch (const std::runtime_error &e) {
		PLOG_DEBUG << e.what();
		ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::attempt, this));
		return;
	}

	// Use the connect attempt delay as timeout, if there are more addresses to attempt and a
	// connect attempt delay is configured, i.e. we are performing "Happy Eyeballs"
	std::optional<std::chrono::milliseconds> timeout = mConnectTimeout;
	if (!isLastAttempt && mConnectAttemptDelay.has_value()) {
		timeout = mConnectAttemptDelay;
		// Make sure not to exceed the connection timeout.
		if (mConnectTimeout.has_value())
			timeout = std::min(*timeout, *mConnectTimeout);
	}

	bool isDelayTimeout =
	    timeout.has_value() && (!mConnectTimeout.has_value() || *timeout < *mConnectTimeout);

	PLOG_VERBOSE << "Polling socket with descriptor " << sock;
	PLOG_VERBOSE_IF(timeout.has_value()) << "Using timeout " << timeout->count() << "ms"
	                                     << (isDelayTimeout ? " (connection attempt delay)" : "");

	PollService::Instance().add(
	    sock, {PollService::Direction::Out, timeout,
	           weak_bind(&TcpTransport::processConnect, this, _1, sock, isDelayTimeout)});
}

socket_t TcpTransport::createSocket(const struct sockaddr *addr, socklen_t addrlen) {
	socket_t sock = INVALID_SOCKET;

	try {
		char node[MAX_NUMERICNODE_LEN];
		char serv[MAX_NUMERICSERV_LEN];
		if (getnameinfo(addr, addrlen, node, MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,
		                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
			PLOG_DEBUG << "Trying address " << node << ":" << serv;
		}

		PLOG_VERBOSE << "Creating TCP socket";

		// Create socket
		sock = ::socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
			throw std::runtime_error("TCP socket creation failed");

		// Configure socket
		configureSocket(sock);

		// Initiate connection
		int ret = ::connect(sock, addr, addrlen);
		if (ret < 0 && sockerrno != SEINPROGRESS && sockerrno != SEWOULDBLOCK) {
			std::ostringstream msg;
			msg << "TCP connection to " << node << ":" << serv << " failed, errno=" << sockerrno;
			throw std::runtime_error(msg.str());
		}

		PLOG_VERBOSE << "Successfully created TCP socket with descriptor " << sock;

	} catch (...) {
		if (sock != INVALID_SOCKET) {
			::closesocket(sock);
			sock = INVALID_SOCKET;
		}
		throw;
	}

	assert(sock != INVALID_SOCKET);
	return sock;
}

void TcpTransport::configureSocket(socket_t sock) {
	// Set non-blocking
	ctl_t nbio = 1;
	if (::ioctlsocket(sock, FIONBIO, &nbio) < 0)
		throw std::runtime_error("Failed to set socket non-blocking mode");

	// Disable the Nagle algorithm
	int nodelay = 1;
	::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay),
	             sizeof(nodelay));

#ifdef __APPLE__
	// MacOS lacks MSG_NOSIGNAL and requires SO_NOSIGPIPE instead
	const sockopt_t enabled = 1;
	if (::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
		throw std::runtime_error("Failed to disable SIGPIPE for socket");
#endif
}

void TcpTransport::setPoll(PollService::Direction direction) {
	PollService::Instance().add(
	    mSock, {direction, direction == PollService::Direction::In ? mReadTimeout : nullopt,
	            weak_bind(&TcpTransport::process, this, _1)});
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
				if (size < message->size())
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
			std::lock_guard lock(mSendMutex);
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

void TcpTransport::closeUnusedSockets() {
	// mSendMutex must be locked

	for (socket_t sock : mSocks) {
		if (mSock == INVALID_SOCKET || sock != mSock) {
			PLOG_VERBOSE << "Closing unused socket with descriptor " << sock;
			PollService::Instance().remove(sock);
			::closesocket(sock);
		}
	}

	mSocks.clear();
	mPendingSocks = 0;
}

void TcpTransport::processConnect(PollService::Event event, socket_t sock, bool isDelayTimeout) {

	if (event == PollService::Event::Timeout && isDelayTimeout) {

		// There are more addresses to attempt concurrently. Add this socket back to the poll
		// service and use the remaining time until the connection timeout as the timeout

		assert(mConnectAttemptDelay.has_value());
		assert(!mConnectTimeout.has_value() || mConnectAttemptDelay < mConnectTimeout);

		std::optional<std::chrono::milliseconds> timeout;
		if (mConnectTimeout.has_value() && mConnectAttemptDelay.has_value())
			timeout = *mConnectTimeout - *mConnectAttemptDelay;

		PLOG_VERBOSE << "Continuing to poll socket with descriptor " << sock;
		PLOG_VERBOSE_IF(timeout.has_value()) << "Using timeout " << timeout->count() << "ms";

		PollService::Instance().add(
		    sock, {PollService::Direction::Out, timeout,
		           weak_bind(&TcpTransport::processConnect, this, _1, sock, false)});

		ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::attempt, this));

		return;
	}

	bool isLastPendingSock = false;

	try {
		{
			std::lock_guard lock(mSendMutex);

			assert((state() == State::Connecting && mPendingSocks > 0) ||
			       (state() != State::Connecting && mPendingSocks == 0));

			if (state() == State::Connecting) {
				mPendingSocks -= 1;
				isLastPendingSock = mPendingSocks == 0;
			}

			if (event == PollService::Event::Error)
				throw std::runtime_error("TCP connection failed");

			if (event == PollService::Event::Timeout)
				throw std::runtime_error("TCP connection timeout");

			if (event != PollService::Event::Out)
				return;

			int err = 0;
			socklen_t errlen = sizeof(err);
			if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &errlen) !=
			    0)
				throw std::runtime_error("Failed to get socket error code");

			if (err != 0) {
				std::ostringstream msg;
				msg << "TCP connection failed, errno=" << err;
				throw std::runtime_error(msg.str());
			}

			if (mSock != INVALID_SOCKET) {
				assert(false);
				throw std::logic_error("Already have a valid socket");
			}

			// Use this socket and close all other opened sockets
			mSock = sock;
			closeUnusedSockets();
		}

		// Success
		PLOG_INFO << "TCP connected";
		PLOG_VERBOSE << "Using socket with descriptor " << mSock;

		// We must change the connected state after releasing the mSendMutex lock, since the
		// connected state change handler might e.g. initialize TLS, which would cause a resource
		// deadlock, if we were still holding the lock to mSendMutex.
		changeState(State::Connected);
		setPoll(PollService::Direction::In);

	} catch (const std::exception &e) {
		PLOG_DEBUG << e.what();
		PollService::Instance().remove(sock);

		// Queue the next connection attempt in one of these cases:
		// 1. We connect synchronously (no connection attempt delay is configured)
		// 2. We connect concurrently and this socket failed before the delay timeout was reached
		// 3. We connect concurrently and this was the last pending socket that timed out. This
		//    means that all sockets that were created so far failed and we must proceed either with
		//    queueing a connection attempt for the next address or with handling connection
		//    failure, for the case that there are no more addresses left
		if (!mConnectAttemptDelay.has_value() || event != PollService::Event::Timeout ||
		    isLastPendingSock) {
			ThreadPool::Instance().enqueue(weak_bind(&TcpTransport::attempt, this));
		}
	}
}

} // namespace rtc::impl

#endif
