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
#include "pollservice.hpp"
#include "queue.hpp"
#include "socket.hpp"
#include "transport.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <mutex>
#include <chrono>

namespace rtc::impl {

class TcpTransport : public Transport {
public:
	using amount_callback = std::function<void(size_t amount)>;

	TcpTransport(string hostname, string service, state_callback callback); // active
	TcpTransport(socket_t sock, state_callback callback);                   // passive
	~TcpTransport();

	void onBufferedAmount(amount_callback callback);
	void setReadTimeout(std::chrono::milliseconds readTimeout);

	void start() override;
	bool stop() override;
	bool send(message_ptr message) override;

	void incoming(message_ptr message) override;
	bool outgoing(message_ptr message) override;

	bool isActive() const { return mIsActive; }
	string remoteAddress() const;

private:
	void connect();
	void prepare(const sockaddr *addr, socklen_t addrlen);
	void configureSocket();
	void setPoll(PollService::Direction direction);
	void close();

	bool trySendQueue();
	bool trySendMessage(message_ptr &message);
	void updateBufferedAmount(ptrdiff_t delta);
	void triggerBufferedAmount(size_t amount);

	void process(PollService::Event event);

	const bool mIsActive;
	string mHostname, mService;
	amount_callback mBufferedAmountCallback;
	optional<std::chrono::milliseconds> mReadTimeout;

	socket_t mSock;
	Queue<message_ptr> mSendQueue;
	size_t mBufferedAmount = 0;
	std::mutex mSendMutex;
};

} // namespace rtc::impl

#endif

#endif
