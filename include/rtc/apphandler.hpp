/**
 * Copyright (c) 2026 Apple Inc
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_APP_HANDLER_H
#define RTC_APP_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "rtp.hpp"
#include "utils.hpp"

namespace rtc {

/// Handles RTCP APP packets (payload type 204) as defined in RFC 3550 section 6.7.
class RTC_CPP_EXPORT AppHandler final : public MediaHandler {
	rtc::synchronized_callback<string, uint8_t, binary> mOnApp;

public:
	/// Constructs the AppHandler to notify when RTCP APP packets are received.
	/// @param onApp Callback invoked with (name, subtype, application-dependent data)
	AppHandler(std::function<void(string name, uint8_t subtype, binary data)> onApp);

	void incoming(message_vector &messages, const message_callback &send) override;

	bool sendRtcpApp(SSRC ssrc, const char name[4], uint8_t subtype, const binary &data,
	                 const message_callback &send) override;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_APP_HANDLER_H
