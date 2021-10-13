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

#ifndef RTC_H264_RTP_PACKETIZER_H
#define RTC_H264_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "mediahandlerrootelement.hpp"
#include "nalunit.hpp"
#include "rtppacketizer.hpp"

namespace rtc {

/// RTP packetization of h264 payload
class RTC_CPP_EXPORT H264RtpPacketizer final : public RtpPacketizer,
                                               public MediaHandlerRootElement {
	shared_ptr<NalUnits> splitMessage(binary_ptr message);
	const uint16_t maximumFragmentSize;

public:
	/// Default clock rate for H264 in RTP
	inline static const uint32_t defaultClockRate = 90 * 1000;

	/// NAL unit separator
	enum class Separator {
		Length = RTC_NAL_SEPARATOR_LENGTH, // first 4 bytes are NAL unit length
		LongStartSequence = RTC_NAL_SEPARATOR_LONG_START_SEQUENCE,   // 0x00, 0x00, 0x00, 0x01
		ShortStartSequence = RTC_NAL_SEPARATOR_SHORT_START_SEQUENCE, // 0x00, 0x00, 0x01
		StartSequence = RTC_NAL_SEPARATOR_START_SEQUENCE, // LongStartSequence or ShortStartSequence
	};

	H264RtpPacketizer(H264RtpPacketizer::Separator separator,
	                  shared_ptr<RtpPacketizationConfig> rtpConfig,
	                  uint16_t maximumFragmentSize = NalUnits::defaultMaximumFragmentSize);

	/// Constructs h264 payload packetizer with given RTP configuration.
	/// @note RTP configuration is used in packetization process which may change some configuration
	/// properties such as sequence number.
	/// @param rtpConfig  RTP configuration
	/// @param maximumFragmentSize maximum size of one NALU fragment
	H264RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
	                  uint16_t maximumFragmentSize = NalUnits::defaultMaximumFragmentSize);

	ChainedOutgoingProduct processOutgoingBinaryMessage(ChainedMessagesProduct messages,
	                                                    message_ptr control) override;

private:
	const Separator separator;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_H264_RTP_PACKETIZER_H */
