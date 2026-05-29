/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_FRAMEINFO_H
#define RTC_FRAMEINFO_H

#include "common.hpp"

#include <chrono>

namespace rtc {

struct RTC_CPP_EXPORT FrameInfo {
	FrameInfo(uint32_t timestamp) : timestamp(timestamp) {};
	template<typename Period = std::ratio<1>> FrameInfo(std::chrono::duration<double, Period> timestamp) : timestampSeconds(timestamp) {}

	[[deprecated]] FrameInfo(uint8_t payloadType, uint32_t timestamp) : timestamp(timestamp), payloadType(payloadType) {};

	uint32_t timestamp = 0;
	uint8_t payloadType = 0;

	optional<std::chrono::duration<double>> timestampSeconds;

	bool isKeyFrame = false;	// Set by the application

	/// Absolute capture timestamp in NTP-format (high 32 bits seconds since
	/// the NTP epoch 1900-01-01 UTC; low 32 bits fractional seconds). When
	/// set alongside an RtpPacketizationConfig with absCaptureTimeId > 0,
	/// the RtpPacketizer writes the 8-byte form of the abs-capture-time RTP
	/// header extension on every packet of this frame. Receivers use this
	/// to recover the source's capture wallclock and compute glass-to-glass
	/// latency.
	/// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/abs-capture-time
	optional<uint64_t> absCaptureTimeNtp;
};

} // namespace rtc

#endif // RTC_FRAMEINFO_H
