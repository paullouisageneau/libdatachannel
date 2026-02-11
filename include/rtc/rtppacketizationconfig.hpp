/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTP_PACKETIZATION_CONFIG_H
#define RTC_RTP_PACKETIZATION_CONFIG_H

#if RTC_ENABLE_MEDIA

#include "dependencydescriptor.hpp"
#include "rtp.hpp"

namespace rtc {

struct RTC_CPP_EXPORT VideoLayersAllocation;

// RTP configuration used in packetization process
class RTC_CPP_EXPORT RtpPacketizationConfig {
public:
	SSRC ssrc;
	std::string cname;
	uint8_t payloadType;
	uint32_t clockRate;
	uint8_t videoOrientationId;

	// current sequence number
	uint16_t sequenceNumber;

	// current timestamp
	uint32_t timestamp;

	// start timestamp
	uint32_t startTimestamp;

	/// Current video orientation
	///
	/// Bit#       7  6  5  4  3  2  1  0
	/// Definition 0  0  0  0  C  F  R1 R0
	///
	/// C
	///   0 - Front-facing camera (use this if unsure)
	///   1 - Back-facing camera
	///
	/// F
	///   0 - No Flip
	///   1 - Horizontal flip
	///
	/// R1 R0 - CW rotation that receiver must apply
	///   0 - 0 degrees
	///   1 - 90 degrees
	///   2 - 180 degrees
	///   3 - 270 degrees
	uint8_t videoOrientation = 0;

	// MID Extension Header
	uint8_t midId = 0;
	optional<std::string> mid;

	// RID Extension Header
	uint8_t ridId = 0;
	optional<std::string> rid;

	// Dependency Descriptor Extension Header
	uint8_t dependencyDescriptorId = 0;

	optional<DependencyDescriptorContext> dependencyDescriptorContext;
	// the negotiated ID of the playout delay header extension
	// https://webrtc.googlesource.com/src/+/main/docs/native-code/rtp-hdrext/playout-delay/README.md
	uint8_t playoutDelayId = 0;

	// Minimum/maxiumum playout delay, in 10ms intervals. A value of 10 would equal a 100ms delay
	uint16_t playoutDelayMin = 0;
	uint16_t playoutDelayMax = 0;

	// Google Video Layers Allocation for simulcast
	// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/video-layers-allocation00
	//
	// The negotiated extension id
	uint8_t videoLayersAllocationId = 0;
	// Stream index, unique per RID/SSRC
	uint8_t videoLayersAllocationStreamIndex = 0;
	// Shared data about layers
	std::shared_ptr<const VideoLayersAllocation> videoLayersAllocationStreams;

	// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/color-space/
	uint8_t colorSpaceId = 0;               // the negotiated ID of color space header extension
	uint8_t colorChromaSitingHorz = 0;      // unspecified
	uint8_t colorChromaSitingVert = 0;      // unspecified
	uint8_t colorRange = 2;                 // full range
	uint8_t colorPrimaries = 1;             // BT.709-6
	uint8_t colorTransfer = 1;              // BT.709-6
	uint8_t colorMatrix = 1;                // BT.709-6

	/// Construct RTP configuration used in packetization process
	/// @param ssrc SSRC of source
	/// @param cname CNAME of source
	/// @param payloadType Payload type of source
	/// @param clockRate Clock rate of source used in timestamps
	/// nullopt)
	/// @param videoOrientationId Video orientation (see above)
	RtpPacketizationConfig(SSRC ssrc, std::string cname, uint8_t payloadType, uint32_t clockRate,
	                       uint8_t videoOrientationId = 0);

	RtpPacketizationConfig(const RtpPacketizationConfig &) = delete;

	/// Convert timestamp to seconds
	/// @param timestamp Timestamp
	/// @param clockRate Clock rate for timestamp calculation
	static double getSecondsFromTimestamp(uint32_t timestamp, uint32_t clockRate);

	/// Convert timestamp to seconds
	/// @param timestamp Timestamp
	double timestampToSeconds(uint32_t timestamp);

	/// Convert seconds to timestamp
	/// @param seconds Number of seconds
	/// @param clockRate Clock rate for timestamp calculation
	static uint32_t getTimestampFromSeconds(double seconds, uint32_t clockRate);

	/// Convert seconds to timestamp
	/// @param seconds Number of seconds
	uint32_t secondsToTimestamp(double seconds);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTP_PACKETIZATION_CONFIG_H */
