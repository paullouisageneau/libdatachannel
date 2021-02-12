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

#ifndef RTC_WEBSOCKET_H
#define RTC_WEBSOCKET_H

#if RTC_ENABLE_WEBSOCKET

#include "channel.hpp"
#include "include.hpp"
#include "init.hpp"
#include "message.hpp"
#include "queue.hpp"

#include <atomic>
#include <optional>
#include <thread>
#include <variant>

namespace rtc {

class TcpTransport;
class TlsTransport;
class WsTransport;

class RTC_CPP_EXPORT WebSocket final : public Channel,
                                       public std::enable_shared_from_this<WebSocket> {
public:
	enum class State : int {
		Connecting = 0,
		Open = 1,
		Closing = 2,
		Closed = 3,
	};

	struct RTC_CPP_EXPORT Configuration {
		bool disableTlsVerification = false; // if true, don't verify the TLS certificate
		std::vector<string> protocols;
	};

	WebSocket(std::optional<Configuration> config = nullopt);
	~WebSocket();

	State readyState() const;

	void open(const string &url);
	void close() override;
	bool send(const message_variant data) override;
	bool send(const byte *data, size_t size) override;

	bool isOpen() const override;
	bool isClosed() const override;
	size_t maxMessageSize() const override;

	// Extended API
	std::optional<message_variant> receive() override;
	std::optional<message_variant> peek() override;
	size_t availableAmount() const override; // total size available to receive

private:
	bool changeState(State state);
	void remoteClose();
	bool outgoing(message_ptr message);
	void incoming(message_ptr message);

	std::shared_ptr<TcpTransport> initTcpTransport();
	std::shared_ptr<TlsTransport> initTlsTransport();
	std::shared_ptr<WsTransport> initWsTransport();
	void closeTransports();

	init_token mInitToken = Init::Token();

	std::shared_ptr<TcpTransport> mTcpTransport;
	std::shared_ptr<TlsTransport> mTlsTransport;
	std::shared_ptr<WsTransport> mWsTransport;
	std::recursive_mutex mInitMutex;

	const Configuration mConfig;
	string mScheme, mHost, mHostname, mService, mPath;
	std::atomic<State> mState = State::Closed;

	Queue<message_ptr> mRecvQueue;
};
} // namespace rtc

#endif

#endif // RTC_WEBSOCKET_H
