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

#ifndef RTC_RTP_PACKETIZATION_CONFIG_H
#define RTC_RTP_PACKETIZATION_CONFIG_H

#if RTC_ENABLE_MEDIA

#include "rtp.hpp"

namespace rtc {

/// RTP configuration used in packetization process
class RTC_CPP_EXPORT RtpPacketizationConfig {
	uint32_t mStartTimestamp = 0;
	double mStartTime = 0;
	RtpPacketizationConfig(const RtpPacketizationConfig &) = delete;

public:
	const SSRC ssrc;
	const std::string cname;
	const uint8_t payloadType;
	const uint32_t clockRate;
	const double &startTime = mStartTime;
	const uint32_t &startTimestamp = mStartTimestamp;
	const uint8_t videoOrientationId;

	/// current sequence number
	uint16_t sequenceNumber;

	/// current timestamp
	uint32_t timestamp;

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

	// For backward compatibility, do not use
	const double &startTime_s = mStartTime;

	enum class EpochStart : uint64_t {
		T1970 = 2208988800, // number of seconds between 1970 and 1900
		T1900 = 0
	};

	/// Creates relation between time and timestamp mapping given start time and start timestamp
	/// @param startTime Start time of the stream
	/// @param epochStart Type of used epoch
	/// @param startTimestamp Corresponding timestamp for given start time (current timestamp will
	/// be used if value is nullopt)
	void setStartTime(double startTime, EpochStart epochStart,
	                  optional<uint32_t> startTimestamp = std::nullopt);

	/// Construct RTP configuration used in packetization process
	/// @param ssrc SSRC of source
	/// @param cname CNAME of source
	/// @param payloadType Payload type of source
	/// @param clockRate Clock rate of source used in timestamps
	/// @param sequenceNumber Initial sequence number of RTP packets (random number is choosed if
	/// nullopt)
	/// @param timestamp Initial timastamp of RTP packets (random number is choosed if nullopt)
	RtpPacketizationConfig(SSRC ssrc, std::string cname, uint8_t payloadType, uint32_t clockRate,
	                       optional<uint16_t> sequenceNumber = std::nullopt,
	                       optional<uint32_t> timestamp = std::nullopt,
	                       uint8_t videoOrientationId = 0);

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
