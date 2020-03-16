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

#include "tcptransport.hpp"

namespace rtc {

using std::to_string;

TcpTransport::TcpTransport(const string &hostname, const string &service, state_callback callback)
    : Transport(nullptr, std::move(callback)), mHostname(hostname), mService(service) {

	PLOG_DEBUG << "Initializing TCP transport";
	mThread = std::thread(&TcpTransport::runLoop, this);
}

TcpTransport::~TcpTransport() { stop(); }

bool TcpTransport::stop() {
	if (!Transport::stop())
		return false;

	close();
	mThread.join();
	return true;
}

bool TcpTransport::send(message_ptr message) { return outgoing(message); }

void TcpTransport::incoming(message_ptr message) { recv(message); }

bool TcpTransport::outgoing(message_ptr message) {
	if (mSock == INVALID_SOCKET)
		throw std::runtime_error("Not connected");

	if (!message)
		return mSendQueue.empty();

	// If nothing is pending, try to send directly
	// It's safe because if the queue is empty, the thread is not sending
	if (mSendQueue.empty() && trySendMessage(message))
		return true;

	mSendQueue.push(message);
	interruptSelect(); // so the thread waits for writability
	return false;
}

void TcpTransport::connect(const string &hostname, const string &service) {
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_ADDRCONFIG;

	struct addrinfo *result = nullptr;
	if (getaddrinfo(hostname.c_str(), service.c_str(), &hints, &result))
		throw std::runtime_error("Resolution failed for \"" + hostname + ":" + service + "\"");

	for (auto p = result; p; p = p->ai_next)
		try {
			connect(p->ai_addr, p->ai_addrlen);
			freeaddrinfo(result);
			return;
		} catch (const std::runtime_error &e) {
			PLOG_WARNING << e.what();
		}

	freeaddrinfo(result);
	throw std::runtime_error("Connection failed to \"" + hostname + ":" + service + "\"");
}

void TcpTransport::connect(const sockaddr *addr, socklen_t addrlen) {
	try {
		// Create socket
		mSock = ::socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
		if (mSock == INVALID_SOCKET)
			throw std::runtime_error("TCP socket creation failed");

		ctl_t b = 1;
		if (::ioctlsocket(mSock, FIONBIO, &b) < 0)
			throw std::runtime_error("Failed to set socket non-blocking mode");

		// Initiate connection
		::connect(mSock, addr, addrlen);

		fd_set writefds;
		FD_ZERO(&writefds);
		FD_SET(mSock, &writefds);
		struct timeval tv;
		tv.tv_sec = 10; // TODO
		tv.tv_usec = 0;
		int ret = ::select(SOCKET_TO_INT(mSock) + 1, NULL, &writefds, NULL, &tv);

		if (ret < 0)
			throw std::runtime_error("Failed to wait for socket connection");

		if (ret == 0 || ::send(mSock, NULL, 0, MSG_NOSIGNAL) != 0)
			throw std::runtime_error("Connection failed");

	} catch (...) {
		if (mSock != INVALID_SOCKET) {
			::closesocket(mSock);
			mSock = INVALID_SOCKET;
		}
		throw;
	}
}

void TcpTransport::close() {
	if (mSock != INVALID_SOCKET) {
		::closesocket(mSock);
		mSock = INVALID_SOCKET;
	}
	changeState(State::Disconnected);
}

bool TcpTransport::trySendQueue() {
	while (auto next = mSendQueue.peek()) {
		auto message = *next;
		if (!trySendMessage(message)) {
			mSendQueue.exchange(message);
			return false;
		}
		mSendQueue.pop();
	}
	return true;
}

bool TcpTransport::trySendMessage(message_ptr &message) {
	auto data = reinterpret_cast<const char *>(message->data());
	auto size = message->size();
	while (size) {
		int len = ::send(mSock, data, size, MSG_NOSIGNAL);
		if (len >= 0) {
			data += len;
			size -= len;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			message = make_message(message->data() + len, message->data() + size);
			return false;
		} else {
			throw std::runtime_error("Connection lost, errno=" + to_string(sockerrno));
		}
	}
	message = nullptr;
	return true;
}

void TcpTransport::runLoop() {
	const size_t bufferSize = 4096;

	changeState(State::Connecting);

	// Connect
	try {
		connect(mHostname, mService);

	} catch (const std::exception &e) {
		PLOG_ERROR << "TCP connect: " << e.what();
		changeState(State::Failed);
		return;
	}

	changeState(State::Connected);

	// Receive loop
	try {
		while (true) {
			fd_set readfds, writefds;
			int n = prepareSelect(readfds, writefds);
			int ret = ::select(n, &readfds, &writefds, NULL, NULL);
			if (ret < 0)
				throw std::runtime_error("Failed to wait on socket");

			if (FD_ISSET(mSock, &readfds)) {
				char buffer[bufferSize];
				int len = ::recv(mSock, buffer, bufferSize, 0);
				if (len < 0)
					throw std::runtime_error("Connection lost, errno=" + to_string(sockerrno));

				if (len == 0)
					break; // clean close

				auto *b = reinterpret_cast<byte *>(buffer);
				incoming(make_message(b, b + len));
			}

			if (FD_ISSET(mSock, &writefds))
				trySendQueue();
		}
	} catch (const std::exception &e) {
		PLOG_ERROR << "TCP recv: " << e.what();
	}

	PLOG_INFO << "TCP disconnected";
	changeState(State::Disconnected);
	recv(nullptr);
}

int TcpTransport::prepareSelect(fd_set &readfds, fd_set &writefds) {
	std::lock_guard lock(mInterruptMutex);
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_SET(mSock, &readfds);

	if (!mSendQueue.empty())
		FD_SET(mSock, &writefds);

	if (mInterruptSock == INVALID_SOCKET)
		mInterruptSock = ::socket(AF_INET, SOCK_DGRAM, 0);

	FD_SET(mInterruptSock, &readfds);
	return std::max(SOCKET_TO_INT(mSock), SOCKET_TO_INT(mInterruptSock)) + 1;
}

void TcpTransport::interruptSelect() {
	std::lock_guard lock(mInterruptMutex);
	if (mInterruptSock != INVALID_SOCKET) {
		::closesocket(mInterruptSock);
		mInterruptSock = INVALID_SOCKET;
	}
}

} // namespace rtc

#endif
