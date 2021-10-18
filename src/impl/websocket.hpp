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

#ifndef RTC_IMPL_WEBSOCKET_H
#define RTC_IMPL_WEBSOCKET_H

#if RTC_ENABLE_WEBSOCKET

#include "channel.hpp"
#include "common.hpp"
#include "init.hpp"
#include "message.hpp"
#include "queue.hpp"
#include "tcptransport.hpp"
#include "tlstransport.hpp"
#include "wstransport.hpp"

#include "rtc/websocket.hpp"

#include <atomic>
#include <thread>

namespace rtc::impl {

struct WebSocket final : public Channel, public std::enable_shared_from_this<WebSocket> {
	using State = rtc::WebSocket::State;
	using Configuration = rtc::WebSocket::Configuration;

	WebSocket(optional<Configuration> optConfig = nullopt, certificate_ptr certificate = nullptr);
	~WebSocket();

	void open(const string &url);
	void close();
	bool outgoing(message_ptr message);
	void incoming(message_ptr message);

	optional<message_variant> receive() override;
	optional<message_variant> peek() override;
	size_t availableAmount() const override;

	bool isOpen() const;
	bool isClosed() const;
	size_t maxMessageSize() const;

	bool changeState(State state);
	void remoteClose();

	shared_ptr<TcpTransport> setTcpTransport(shared_ptr<TcpTransport> transport);
	shared_ptr<TlsTransport> initTlsTransport();
	shared_ptr<WsTransport> initWsTransport();
	shared_ptr<TcpTransport> getTcpTransport() const;
	shared_ptr<TlsTransport> getTlsTransport() const;
	shared_ptr<WsTransport> getWsTransport() const;
	shared_ptr<WsHandshake> getWsHandshake() const;

	void closeTransports();

	const Configuration config;

	std::atomic<State> state = State::Closed;

private:
	const init_token mInitToken = Init::Instance().token();

	const certificate_ptr mCertificate;
	bool mIsSecure;

	optional<string> mHostname; // for TLS SNI

	shared_ptr<TcpTransport> mTcpTransport;
	shared_ptr<TlsTransport> mTlsTransport;
	shared_ptr<WsTransport> mWsTransport;
	shared_ptr<WsHandshake> mWsHandshake;

	Queue<message_ptr> mRecvQueue;
};

} // namespace rtc::impl

#endif

#endif // RTC_IMPL_WEBSOCKET_H
