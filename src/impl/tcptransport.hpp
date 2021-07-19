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

#ifndef RTC_IMPL_TCP_TRANSPORT_H
#define RTC_IMPL_TCP_TRANSPORT_H

#include "common.hpp"
#include "queue.hpp"
#include "selectinterrupter.hpp"
#include "socket.hpp"
#include "transport.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <mutex>
#include <thread>

namespace rtc::impl {

class TcpTransport : public Transport {
public:
	TcpTransport(string hostname, string service, state_callback callback); // active
	TcpTransport(socket_t sock, state_callback callback);                   // passive
	~TcpTransport();

	void start() override;
	bool stop() override;
	bool send(message_ptr message) override;

	void incoming(message_ptr message) override;
	bool outgoing(message_ptr message) override;

	bool isActive() const { return mIsActive; }

	string remoteAddress() const;

private:
	void connect(const string &hostname, const string &service);
	void connect(const sockaddr *addr, socklen_t addrlen);
	void close();

	bool trySendQueue();
	bool trySendMessage(message_ptr &message);

	void runLoop();

	const bool mIsActive;
	string mHostname, mService;

	socket_t mSock = INVALID_SOCKET;
	std::mutex mSockMutex;
	std::thread mThread;
	SelectInterrupter mInterrupter;
	Queue<message_ptr> mSendQueue;
};

} // namespace rtc::impl

#endif

#endif
