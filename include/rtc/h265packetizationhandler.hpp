/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_H265_PACKETIZATION_HANDLER_H
#define RTC_H265_PACKETIZATION_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "h265rtppacketizer.hpp"
#include "mediachainablehandler.hpp"
#include "h265nalunit.hpp"

namespace rtc {

/// Handler for H265 packetization
class RTC_CPP_EXPORT H265PacketizationHandler final : public MediaChainableHandler {
public:
	/// Construct handler for H265 packetization.
	/// @param packetizer RTP packetizer for h265
	H265PacketizationHandler(shared_ptr<H265RtpPacketizer> packetizer);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_H265_PACKETIZATION_HANDLER_H */
