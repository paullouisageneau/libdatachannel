/*
 * libdatachannel streamer example
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

#ifndef OpusRTPPacketizer_hpp
#define OpusRTPPacketizer_hpp

#if RTC_ENABLE_MEDIA

#include "rtppacketizer.hpp"

namespace rtc {

/// RTP packetizer for opus
class RTC_CPP_EXPORT OpusRTPPacketizer: public rtc::RTPPacketizer {

public:
    /// default clock rate used in opus RTP communication
    static const uint32_t defaultClockRate = 48 * 1000;

    /// Constructs opus packetizer with given RTP configuration.
    /// @note RTP configuration is used in packetization process which may change some configuration properties such as sequence number.
    /// @param rtpConfig  RTP configuration
    OpusRTPPacketizer(std::shared_ptr<rtc::RTPPacketizationConfig> rtpConfig);

    /// Creates RTP packet for given payload based on `rtpConfig`.
    /// @note This function increase sequence number after packetization.
    /// @param payload RTP payload
    /// @param setMark This needs to be `false` for all RTP packets with opus payload
    rtc::message_ptr packetize(rtc::binary payload, bool setMark) override;
};

} // namespace

#endif /* RTC_ENABLE_MEDIA */

#endif /* OpusRTPPacketizer_hpp */
