/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_ICE_UDP_MUX_LISTENER_H
#define RTC_ICE_UDP_MUX_LISTENER_H

#include "common.hpp"

namespace rtc {

namespace impl {

struct IceUdpMuxListener;

} // namespace impl

struct IceUdpMuxRequest { // TODO change name
	string localUfrag;
	string remoteUfrag;
	string remoteAddress;
	uint16_t remotePort;
};

class RTC_CPP_EXPORT IceUdpMuxListener final : private CheshireCat<impl::IceUdpMuxListener> {
public:
	IceUdpMuxListener(uint16_t port, optional<string> bindAddress = nullopt);
	~IceUdpMuxListener();

	void stop();

	uint16_t port() const;

	void OnUnhandledStunRequest(std::function<void(IceUdpMuxRequest)> callback);

private:
	using CheshireCat<impl::IceUdpMuxListener>::impl;
};

} // namespace rtc

#endif
