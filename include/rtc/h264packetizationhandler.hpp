/**
 * Copyright (c) 2020 Filip Klembara (in2core)
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

#ifndef RTC_H264_PACKETIZATION_HANDLER_H
#define RTC_H264_PACKETIZATION_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "h264rtppacketizer.hpp"
#include "mediachainablehandler.hpp"
#include "nalunit.hpp"

namespace rtc {

/// Handler for H264 packetization
class RTC_CPP_EXPORT H264PacketizationHandler final : public MediaChainableHandler {
public:
	/// Construct handler for H264 packetization.
	/// @param packetizer RTP packetizer for h264
	H264PacketizationHandler(shared_ptr<H264RtpPacketizer> packetizer);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_H264_PACKETIZATION_HANDLER_H */
