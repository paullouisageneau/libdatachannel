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

#ifndef RTC_TCP_TRANSPORT_H
#define RTC_TCP_TRANSPORT_H

#include "include.hpp"
#include "queue.hpp"
#include "transport.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <mutex>
#include <thread>

// Use the socket defines from libjuice
#include "../deps/libjuice/src/socket.h"

namespace rtc {

// Utility class to interrupt select()
class SelectInterrupter {
public:
	SelectInterrupter();
	~SelectInterrupter();

	int prepare(fd_set &readfds, fd_set &writefds);
	void interrupt();

private:
	std::mutex mMutex;
#ifdef _WIN32
	socket_t mDummySock = INVALID_SOCKET;
#else // assume POSIX
	int mPipeIn, mPipeOut;
#endif
};

class TcpTransport : public Transport {
public:
	TcpTransport(const string &hostname, const string &service, state_callback callback);
	~TcpTransport();

	bool stop() override;
	bool send(message_ptr message) override;

	void incoming(message_ptr message) override;
	bool outgoing(message_ptr message) override;

private:
	void connect(const string &hostname, const string &service);
	void connect(const sockaddr *addr, socklen_t addrlen);
	void close();

	bool trySendQueue();
	bool trySendMessage(message_ptr &message);

	void runLoop();

	int prepareSelect(fd_set &readfds, fd_set &writefds);
	void interruptSelect();

	string mHostname, mService;

	socket_t mSock = INVALID_SOCKET;
	std::thread mThread;
	SelectInterrupter mInterrupter;
	Queue<message_ptr> mSendQueue;
};

} // namespace rtc

#endif

#endif
