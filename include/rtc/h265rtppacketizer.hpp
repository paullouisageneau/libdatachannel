/**
 * Copyright (c) 2023 Zita Liao (Dolby)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_H265_RTP_PACKETIZER_H
#define RTC_H265_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "h265nalunit.hpp"
#include "rtppacketizer.hpp"

namespace rtc {

// RTP packetization for H265
class RTC_CPP_EXPORT H265RtpPacketizer final : public RtpPacketizer {
public:
	using Separator = NalUnit::Separator;

	inline static const uint32_t ClockRate = VideoClockRate;
	[[deprecated("Use ClockRate")]] inline static const uint32_t defaultClockRate = ClockRate;

	// Constructs h265 payload packetizer with given RTP configuration.
	// @note RTP configuration is used in packetization process which may change some configuration
	// properties such as sequence number.
	// @param separator NAL unit separator
	// @param rtpConfig  RTP configuration
	// @param maxFragmentSize maximum size of one NALU fragment
	H265RtpPacketizer(Separator separator, shared_ptr<RtpPacketizationConfig> rtpConfig,
	                  size_t maxFragmentSize = DefaultMaxFragmentSize);

	// For backward compatibility, do not use
	[[deprecated]] H265RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
	                  size_t maxFragmentSize = DefaultMaxFragmentSize);

private:
	std::vector<binary> fragment(binary data) override;
	std::vector<H265NalUnit> splitFrame(const binary &frame);

	const NalUnit::Separator mSeparator;
	const size_t mMaxFragmentSize;
};

// For backward compatibility, do not use
using H265PacketizationHandler [[deprecated("Add H265RtpPacketizer directly")]] = PacketizationHandler;

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_H265_RTP_PACKETIZER_H */
