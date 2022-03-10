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

#include "pollinterrupter.hpp"
#include "internals.hpp"

#if RTC_ENABLE_WEBSOCKET

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rtc::impl {

PollInterrupter::PollInterrupter() {
#ifdef _WIN32
	struct addrinfo *ai = NULL;
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	if (getaddrinfo("localhost", "0", &hints, &ai) != 0)
		throw std::runtime_error("Resolution failed for localhost address");

	try {
		mSock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (mSock == INVALID_SOCKET)
			throw std::runtime_error("UDP socket creation failed");

		// Set non-blocking
		ctl_t nbio = 1;
		::ioctlsocket(mSock, FIONBIO, &nbio);

		// Bind
		if (::bind(mSock, ai->ai_addr, (socklen_t)ai->ai_addrlen) < 0)
			throw std::runtime_error("Failed to bind UDP socket");

		// Connect to self
		struct sockaddr_storage addr;
		socklen_t addrlen = sizeof(addr);
		if (::getsockname(mSock, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) < 0)
			throw std::runtime_error("getsockname failed");

		if (::connect(mSock, reinterpret_cast<struct sockaddr *>(&addr), addrlen) < 0)
			throw std::runtime_error("Failed to connect UDP socket");

	} catch (...) {
		freeaddrinfo(ai);
		if (mSock != INVALID_SOCKET)
			::closesocket(mSock);

		throw;
	}

	freeaddrinfo(ai);

#else
	int pipefd[2];
	if (::pipe(pipefd) != 0)
		throw std::runtime_error("Failed to create pipe");

	::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	::fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
	mPipeOut = pipefd[1]; // read
	mPipeIn = pipefd[0];  // write
#endif
}

PollInterrupter::~PollInterrupter() {
#ifdef _WIN32
	::closesocket(mSock);
#else
	::close(mPipeIn);
	::close(mPipeOut);
#endif
}

void PollInterrupter::prepare(struct pollfd &pfd) {
#ifdef _WIN32
	pfd.fd = mSock;
#else
	pfd.fd = mPipeIn;
#endif
	pfd.events = POLLIN;
}

void PollInterrupter::process(struct pollfd &pfd) {
	if (pfd.revents & POLLIN) {
#ifdef _WIN32
		char dummy;
		while (::recv(pfd.fd, &dummy, 1, 0) >= 0) {
			// Ignore
		}
#else
		char dummy;
		while (::read(pfd.fd, &dummy, 1) > 0) {
			// Ignore
		}
#endif
	}
}

void PollInterrupter::interrupt() {
#ifdef _WIN32
	if (::send(mSock, NULL, 0, 0) < 0 && sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK) {
		PLOG_WARNING << "Writing to interrupter socket failed, errno=" << sockerrno;
	}
#else
	char dummy = 0;
	if (::write(mPipeOut, &dummy, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		PLOG_WARNING << "Writing to interrupter pipe failed, errno=" << errno;
	}
#endif
}

} // namespace rtc::impl

#endif
