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

#ifndef RTC_RTP_PACKETIZER_H
#define RTC_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "message.hpp"
#include "rtppacketizationconfig.hpp"

namespace rtc {

/// Class responsible for RTP packetization
class RTC_CPP_EXPORT RtpPacketizer {
	static const auto rtpHeaderSize = 12;

public:
	// RTP configuration
	const std::shared_ptr<RtpPacketizationConfig> rtpConfig;

	/// Constructs packetizer with given RTP configuration.
	/// @note RTP configuration is used in packetization process which may change some configuration
	/// properties such as sequence number.
	/// @param rtpConfig  RTP configuration
	RtpPacketizer(std::shared_ptr<RtpPacketizationConfig> rtpConfig);

	/// Creates RTP packet for given payload based on `rtpConfig`.
	/// @note This function increase sequence number after packetization.
	/// @param payload RTP payload
	/// @param setMark Set marker flag in RTP packet if true
	virtual std::shared_ptr<binary> packetize(std::shared_ptr<binary> payload, bool setMark);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTP_PACKETIZER_H */
