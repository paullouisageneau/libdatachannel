/*
 * libdatachannel client example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef H264_PACKETIZATION_HANDLER_H
#define H264_PACKETIZATION_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "h264rtppacketizer.hpp"
#include "nalunit.hpp"
#include "mediachainablehandler.hpp"

namespace rtc {

/// Handler for H264 packetization
class RTC_CPP_EXPORT H264PacketizationHandler : public MediaChainableHandler {
public:
	/// Construct handler for H264 packetization.
	/// @param packetizer RTP packetizer for h264
	H264PacketizationHandler(shared_ptr<H264RtpPacketizer> packetizer);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* H264_PACKETIZATION_HANDLER_H */
