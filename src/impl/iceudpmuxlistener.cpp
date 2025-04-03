/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "iceudpmuxlistener.hpp"
#include "internals.hpp"

namespace rtc::impl {

#if !USE_NICE
void IceUdpMuxListener::UnhandledStunRequestCallback(const juice_mux_binding_request *info,
                                                     void *user_ptr) {
	auto listener = static_cast<IceUdpMuxListener *>(user_ptr);
	if (!listener)
		return;

	IceUdpMuxRequest request;
	request.localUfrag = info->local_ufrag;
	request.remoteUfrag = info->remote_ufrag;
	request.remoteAddress = info->address;
	request.remotePort = info->port;
	listener->unhandledStunRequestCallback(std::move(request));
}
#endif

IceUdpMuxListener::IceUdpMuxListener(uint16_t port, [[maybe_unused]] optional<string> bindAddress) : port(port) {
	PLOG_VERBOSE << "Creating IceUdpMuxListener";

#if !USE_NICE
	PLOG_DEBUG << "Registering ICE UDP mux listener for port " << port;
	if (juice_mux_listen(bindAddress ? bindAddress->c_str() : NULL, port,
	                     IceUdpMuxListener::UnhandledStunRequestCallback, this) < 0) {
		throw std::runtime_error("Failed to register ICE UDP mux listener");
	}
#else
	PLOG_WARNING << "ICE UDP mux is not available with libnice";
#endif
}

IceUdpMuxListener::~IceUdpMuxListener() {
	PLOG_VERBOSE << "Destroying IceUdpMuxListener";
	stop();
}

void IceUdpMuxListener::stop() {
	if (mStopped.exchange(true))
		return;

#if !USE_NICE
	PLOG_DEBUG << "Unregistering ICE UDP mux listener for port " << port;
	if (juice_mux_listen(NULL, port, NULL, NULL) < 0) {
		PLOG_ERROR << "Failed to unregister ICE UDP mux listener";
	}
#endif
}

} // namespace rtc::impl
