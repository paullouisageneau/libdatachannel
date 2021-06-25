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

#ifndef RTC_IMPL_WEBSOCKETSERVER_H
#define RTC_IMPL_WEBSOCKETSERVER_H

#if RTC_ENABLE_WEBSOCKET

#include "certificate.hpp"
#include "common.hpp"
#include "init.hpp"
#include "message.hpp"
#include "tcpserver.hpp"
#include "websocket.hpp"

#include "rtc/websocket.hpp"
#include "rtc/websocketserver.hpp"

#include <atomic>
#include <thread>

namespace rtc::impl {

struct WebSocketServer final : public std::enable_shared_from_this<WebSocketServer> {
	using Configuration = rtc::WebSocketServer::Configuration;

	WebSocketServer(Configuration config_);
	~WebSocketServer();

	void stop();

	const Configuration config;
	const unique_ptr<TcpServer> tcpServer;

	synchronized_callback<shared_ptr<rtc::WebSocket>> clientCallback;

private:
	const init_token mInitToken = Init::Token();

	void runLoop();

	certificate_ptr mCertificate;
	std::thread mThread;
	std::atomic<bool> mStopped;
};

} // namespace rtc::impl

#endif

#endif // RTC_IMPL_WEBSOCKET_H
