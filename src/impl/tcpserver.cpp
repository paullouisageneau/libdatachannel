/**
 * Copyright (c) 2021 Paul-Louis Ageneau
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

#include "tcpserver.hpp"
#include "internals.hpp"

#if RTC_ENABLE_WEBSOCKET

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rtc::impl {

TcpServer::TcpServer(uint16_t port) {
	PLOG_DEBUG << "Initializing TCP server";
	listen(port);
}

TcpServer::~TcpServer() { close(); }

shared_ptr<TcpTransport> TcpServer::accept() {
	while (true) {
		std::unique_lock lock(mSockMutex);

		if (mSock == INVALID_SOCKET)
			break;

		struct pollfd pfd[2];
		pfd[0].fd = mSock;
		pfd[0].events = POLLIN;
		mInterrupter.prepare(pfd[1]);
		lock.unlock();
		int ret = ::poll(pfd, 2, -1);
		lock.lock();

		if (mSock == INVALID_SOCKET)
			break;

		if (ret < 0) {
			if (sockerrno == SEINTR || sockerrno == SEAGAIN) // interrupted
				continue;
			else
				throw std::runtime_error("Failed to wait for socket connection");
		}

		if (pfd[0].revents & POLLNVAL || pfd[0].revents & POLLERR) {
			throw std::runtime_error("Error while waiting for socket connection");
		}

		if (pfd[0].revents & POLLIN) {
			struct sockaddr_storage addr;
			socklen_t addrlen = sizeof(addr);
			socket_t incomingSock = ::accept(mSock, (struct sockaddr *)&addr, &addrlen);

			if (incomingSock != INVALID_SOCKET) {
				return std::make_shared<TcpTransport>(incomingSock, nullptr); // no state callback

			} else if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK) {
				PLOG_ERROR << "TCP server failed, errno=" << sockerrno;
				throw std::runtime_error("TCP server failed");
			}
		}
	}

	PLOG_DEBUG << "TCP server closed";
	return nullptr;
}

void TcpServer::close() {
	std::unique_lock lock(mSockMutex);
	if (mSock != INVALID_SOCKET) {
		PLOG_DEBUG << "Closing TCP server socket";
		::closesocket(mSock);
		mSock = INVALID_SOCKET;
		mInterrupter.interrupt();
	}
}

void TcpServer::listen(uint16_t port) {
	PLOG_DEBUG << "Listening on port " << port;

	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

	struct addrinfo *result = nullptr;
	if (::getaddrinfo(nullptr, std::to_string(port).c_str(), &hints, &result))
		throw std::runtime_error("Resolution failed for local address");

	try {
		static const auto find_family = [](struct addrinfo *ai_list, int family) {
			struct addrinfo *ai = ai_list;
			while (ai && ai->ai_family != family)
				ai = ai->ai_next;
			return ai;
		};

		struct addrinfo *ai;
		if ((ai = find_family(result, AF_INET6)) == NULL &&
		    (ai = find_family(result, AF_INET)) == NULL)
			throw std::runtime_error("No suitable address family found");

		std::unique_lock lock(mSockMutex);
		PLOG_VERBOSE << "Creating TCP server socket";

		// Create socket
		mSock = ::socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if (mSock == INVALID_SOCKET)
			throw std::runtime_error("TCP server socket creation failed");

		// Listen on both IPv6 and IPv4
		const sockopt_t disabled = 0;
		if (ai->ai_family == AF_INET6)
			::setsockopt(mSock, IPPROTO_IPV6, IPV6_V6ONLY,
			             reinterpret_cast<const char *>(&disabled), sizeof(disabled));

		// Set non-blocking
		ctl_t b = 1;
		if (::ioctlsocket(mSock, FIONBIO, &b) < 0)
			throw std::runtime_error("Failed to set socket non-blocking mode");

		// Bind socket
		if (::bind(mSock, ai->ai_addr, ai->ai_addrlen) < 0) {
			PLOG_WARNING << "TCP server socket binding on port " << port
			             << " failed, errno=" << sockerrno;
			throw std::runtime_error("TCP server socket binding failed");
		}

		// Listen
		const int backlog = 10;
		if (::listen(mSock, backlog) < 0) {
			PLOG_WARNING << "TCP server socket listening failed, errno=" << sockerrno;
			throw std::runtime_error("TCP server socket listening failed");
		}

		if (port != 0) {
			mPort = port;
		} else {
			struct sockaddr_storage addr;
			socklen_t addrlen = sizeof(addr);
			if (::getsockname(mSock, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) < 0)
				throw std::runtime_error("getsockname failed");

			switch (addr.ss_family) {
			case AF_INET:
				mPort = ntohs(reinterpret_cast<struct sockaddr_in *>(&addr)->sin_port);
				break;
			case AF_INET6:
				mPort = ntohs(reinterpret_cast<struct sockaddr_in6 *>(&addr)->sin6_port);
				break;
			default:
				throw std::logic_error("Unknown address family");
			}
		}
	} catch (...) {
		freeaddrinfo(result);
		if (mSock != INVALID_SOCKET) {
			::closesocket(mSock);
			mSock = INVALID_SOCKET;
		}
		throw;
	}

	freeaddrinfo(result);
}

} // namespace rtc::impl

#endif
