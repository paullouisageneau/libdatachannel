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

#ifndef H264RTPPacketizer_hpp
#define H264RTPPacketizer_hpp

#if RTC_ENABLE_MEDIA

#include "rtppacketizer.hpp"

namespace rtc {

/// RTP packetization of h264 payload
class RTC_CPP_EXPORT H264RTPPacketizer: public rtc::RTPPacketizer {

public:
    /// Default clock rate for H264 in RTP
    static const auto defaultClockRate = 90 * 1000;

    /// Constructs h264 payload packetizer with given RTP configuration.
    /// @note RTP configuration is used in packetization process which may change some configuration properties such as sequence number.
    /// @param rtpConfig  RTP configuration
    H264RTPPacketizer(std::shared_ptr<rtc::RTPPacketizationConfig> rtpConfig);
};

} // namespace

#endif /* RTC_ENABLE_MEDIA */

#endif /* H264RTPPacketizer_hpp */
