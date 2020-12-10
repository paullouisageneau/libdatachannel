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

#ifndef RTPPacketizer_hpp
#define RTPPacketizer_hpp

#include "rtppacketizationconfig.hpp"
#include "message.hpp"

#if RTC_ENABLE_MEDIA

namespace rtc {

/// Class responsizble for rtp packetization
class RTPPacketizer {
    static const auto rtpHeaderSize = 12;
public:
    // rtp configuration
    const std::shared_ptr<RTPPacketizationConfig> rtpConfig;

    /// Constructs packetizer with given RTP configuration.
    /// @note RTP configuration is used in packetization process which may change some configuration properties such as sequence number.
    /// @param rtpConfig  RTP configuration
    RTPPacketizer(std::shared_ptr<RTPPacketizationConfig> rtpConfig);

    /// Creates RTP packet for given payload based on `rtpConfig`.
    /// @note This function increase sequence number after packetization.
    /// @param payload RTP payload
    /// @param setMark Set marker flag in RTP packet if true
    virtual message_ptr packetize(binary payload, bool setMark);
};

}   // namespace

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTPPacketizer_hpp */
