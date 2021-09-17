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

#ifndef RTC_IMPL_TCP_SERVER_H
#define RTC_IMPL_TCP_SERVER_H

#include "common.hpp"
#include "pollinterrupter.hpp"
#include "queue.hpp"
#include "socket.hpp"
#include "tcptransport.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

class TcpServer {
public:
	TcpServer(uint16_t port);
	~TcpServer();

	shared_ptr<TcpTransport> accept();
	void close();

	uint16_t port() const { return mPort; }

private:
	void listen(uint16_t port);

	uint16_t mPort;
	socket_t mSock = INVALID_SOCKET;
	std::mutex mSockMutex;
	PollInterrupter mInterrupter;
};

} // namespace rtc::impl

#endif

#endif
