/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_ICE_UDP_MUX_LISTENER_H
#define RTC_IMPL_ICE_UDP_MUX_LISTENER_H

#include "common.hpp"

#include "rtc/iceudpmuxlistener.hpp"

#if !USE_NICE
#include <juice/juice.h>
#endif

#include <atomic>

namespace rtc::impl {

struct IceUdpMuxListener final {
	IceUdpMuxListener(uint16_t port, optional<string> bindAddress = nullopt);
	~IceUdpMuxListener();

	void stop();

	const uint16_t port;
	synchronized_callback<IceUdpMuxRequest> unhandledStunRequestCallback;

private:
#if !USE_NICE
	static void UnhandledStunRequestCallback(const juice_mux_binding_request *info, void *user_ptr);
#endif

	std::atomic<bool> mStopped;
};

}

#endif

