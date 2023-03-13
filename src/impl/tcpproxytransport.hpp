/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_TCP_PROXY_TRANSPORT_H
#define RTC_IMPL_TCP_PROXY_TRANSPORT_H

#include "common.hpp"
#include "transport.hpp"
#include "wshandshake.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

class TcpTransport;

class TcpProxyTransport final : public Transport, public std::enable_shared_from_this<TcpProxyTransport> {
public:
	TcpProxyTransport(shared_ptr<TcpTransport> lower, std::string hostname, std::string service,
				state_callback stateCallback);
	~TcpProxyTransport();

	void start() override;
	void stop() override;
	bool send(message_ptr message) override;

	bool isActive() const;

private:
	void incoming(message_ptr message) override;
	bool sendHttpRequest();
	std::string generateHttpRequest();
	size_t parseHttpResponse( std::byte* buffer, size_t size );

	const bool mIsActive;
	std::string mHostname;
	std::string mService;
	binary mBuffer;
	std::mutex mSendMutex;
};

} // namespace rtc::impl

#endif

#endif
