/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "iceudpmuxlistener.hpp"

#include "impl/iceudpmuxlistener.hpp"

namespace rtc {

IceUdpMuxListener::IceUdpMuxListener(uint16_t port, optional<string> bindAddress)
    : CheshireCat<impl::IceUdpMuxListener>(port, std::move(bindAddress)) {}

IceUdpMuxListener::~IceUdpMuxListener() {}

void IceUdpMuxListener::stop() { impl()->stop(); }

uint16_t IceUdpMuxListener::port() const { return impl()->port; }

void IceUdpMuxListener::OnUnhandledStunRequest(std::function<void(IceUdpMuxRequest)> callback) {
	impl()->unhandledStunRequestCallback = callback;
}

} // namespace rtc
