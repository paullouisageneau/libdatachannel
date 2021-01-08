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

#ifndef H264PacketizationHandler_hpp
#define H264PacketizationHandler_hpp

#if RTC_ENABLE_MEDIA

#include "rtcp.hpp"
#include "h264rtppacketizer.hpp"
#include "rtcpsenderreportable.hpp"
#include "nalunit.hpp"

namespace rtc {

/// Handler for H264 packetization
class RTC_CPP_EXPORT H264PacketizationHandler: public RtcpHandler, public RTCPSenderReportable {
    /// RTP packetizer for H264
    const std::shared_ptr<H264RTPPacketizer> packetizer;

    const uint16_t maximumFragmentSize;

    std::shared_ptr<NalUnits> splitMessage(message_ptr message);
public:
    /// Nalunit separator
    enum class Separator {
        LongStartSequence,  // 0x00, 0x00, 0x00, 0x01
        ShortStartSequence, // 0x00, 0x00, 0x01
        StartSequence,      // LongStartSequence or ShortStartSequence
        Length              // first 4 bytes is nal unit length
    };

    /// Construct handler for H264 packetization.
    /// @param separator Nal units separator
    /// @param packetizer RTP packetizer for h264
    H264PacketizationHandler(Separator separator, std::shared_ptr<H264RTPPacketizer> packetizer, uint16_t maximumFragmentSize = NalUnits::defaultMaximumFragmentSize);

    /// Returns message unchanged
    /// @param ptr message
    message_ptr incoming(message_ptr ptr) override;

    /// Returns packetized message if message type is binary
    /// @note NAL units in `ptr` message must be separated by `separator` given in constructor
    /// @note If message generates multiple rtp packets, all but last are send using `outgoingCallback`. It is your responsibility to send last packet.
    /// @param ptr message containing all NAL units for current timestamp (one sample)
    /// @return last packet
    message_ptr outgoing(message_ptr ptr) override;
private:
    /// Separator
    const Separator separator;
};

} // namespace

#endif /* RTC_ENABLE_MEDIA */

#endif /* H264PacketizationHandler_hpp */
