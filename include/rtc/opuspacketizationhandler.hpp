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

#ifndef OpusPacketizationHandler_hpp
#define OpusPacketizationHandler_hpp

#include "rtcpsenderreportable.hpp"
#include "opusrtppacketizer.hpp"
#include "rtcp.hpp"

#if RTC_ENABLE_MEDIA

namespace rtc {

/// Handler for opus packetization
class OpusPacketizationHandler: public RtcpHandler, public RTCPSenderReportable {
    /// RTP packetizer for opus
    const std::shared_ptr<OpusRTPPacketizer> packetizer;

public:
    /// Construct handler for opus packetization.
    /// @param packetizer RTP packetizer for opus
    OpusPacketizationHandler(std::shared_ptr<OpusRTPPacketizer> packetizer);

    /// Returns message unchanged
    /// @param ptr message
    message_ptr incoming(message_ptr ptr) override;
    /// Returns packetized message if message type is binary
    /// @param ptr message
    message_ptr outgoing(message_ptr ptr) override;
};

}   // namespace

#endif /* RTC_ENABLE_MEDIA */

#endif /* OpusPacketizationHandler_hpp */
