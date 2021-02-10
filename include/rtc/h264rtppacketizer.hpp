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

#ifndef H264_RTP_PACKETIZER_H
#define H264_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "nalunit.hpp"
#include "rtppacketizer.hpp"
#include "mediahandlerrootelement.hpp"

namespace rtc {

/// RTP packetization of h264 payload
class RTC_CPP_EXPORT H264RtpPacketizer : public RtpPacketizer, public MediaHandlerRootElement {
	std::shared_ptr<NalUnits> splitMessage(binary_ptr message);
	const uint16_t maximumFragmentSize;

public:
	/// Default clock rate for H264 in RTP
	static const auto defaultClockRate = 90 * 1000;

	/// Nalunit separator
	enum class Separator {
		LongStartSequence,  // 0x00, 0x00, 0x00, 0x01
		ShortStartSequence, // 0x00, 0x00, 0x01
		StartSequence,      // LongStartSequence or ShortStartSequence
		Length              // first 4 bytes is nal unit length
	};

	H264RtpPacketizer(H264RtpPacketizer::Separator separator, std::shared_ptr<RtpPacketizationConfig> rtpConfig,
					  uint16_t maximumFragmentSize = NalUnits::defaultMaximumFragmentSize);

	/// Constructs h264 payload packetizer with given RTP configuration.
	/// @note RTP configuration is used in packetization process which may change some configuration
	/// properties such as sequence number.
	/// @param rtpConfig  RTP configuration
	/// @param maximumFragmentSize maximum size of one NALU fragment
	H264RtpPacketizer(std::shared_ptr<RtpPacketizationConfig> rtpConfig,
					  uint16_t maximumFragmentSize = NalUnits::defaultMaximumFragmentSize);

	ChainedOutgoingProduct processOutgoingBinaryMessage(ChainedMessagesProduct messages, message_ptr control) override;
private:
	const Separator separator;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* H264_RTP_PACKETIZER_H */
