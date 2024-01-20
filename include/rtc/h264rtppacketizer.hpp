/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 * Copyright (c) 2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_H264_RTP_PACKETIZER_H
#define RTC_H264_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "nalunit.hpp"
#include "rtppacketizer.hpp"

namespace rtc {

/// RTP packetization for H264
class RTC_CPP_EXPORT H264RtpPacketizer final : public RtpPacketizer {
public:
	using Separator = NalUnit::Separator;

	/// Default clock rate for H264 in RTP
	inline static const uint32_t defaultClockRate = 90 * 1000;

	/// Constructs h264 payload packetizer with given RTP configuration.
	/// @note RTP configuration is used in packetization process which may change some configuration
	/// properties such as sequence number.
	/// @param separator NAL unit separator
	/// @param rtpConfig RTP configuration
	/// @param maxFragmentSize maximum size of one NALU fragment
	H264RtpPacketizer(Separator separator, shared_ptr<RtpPacketizationConfig> rtpConfig,
	                  uint16_t maxFragmentSize = NalUnits::defaultMaximumFragmentSize);

	// For backward compatibility, do not use
	[[deprecated]] H264RtpPacketizer(
	    shared_ptr<RtpPacketizationConfig> rtpConfig,
	    uint16_t maxFragmentSize = NalUnits::defaultMaximumFragmentSize);

	void outgoing(message_vector &messages, const message_callback &send) override;

private:
	shared_ptr<NalUnits> splitMessage(binary_ptr message);

	const uint16_t maxFragmentSize;
	const Separator separator;
};

// For backward compatibility, do not use
using H264PacketizationHandler [[deprecated("Add H264RtpPacketizer directly")]] = PacketizationHandler;

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_H264_RTP_PACKETIZER_H */
