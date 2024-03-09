/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtppacketizationconfig.hpp"

#include "impl/utils.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <random>

namespace rtc {

namespace utils = impl::utils;

RtpPacketizationConfig::RtpPacketizationConfig(SSRC ssrc, string cname, uint8_t payloadType,
                                               uint32_t clockRate, uint8_t videoOrientationId)
    : ssrc(ssrc), cname(cname), payloadType(payloadType), clockRate(clockRate),
      videoOrientationId(videoOrientationId) {
	assert(clockRate > 0);

	// RFC 3550: The initial value of the sequence number SHOULD be random (unpredictable) to make
	// known-plaintext attacks on encryption more difficult [...] The initial value of the timestamp
	// SHOULD be random, as for the sequence number.
	auto uniform = std::bind(std::uniform_int_distribution<uint32_t>(), utils::random_engine());
	sequenceNumber = static_cast<uint16_t>(uniform());
	timestamp = startTimestamp = uniform();
}

double RtpPacketizationConfig::getSecondsFromTimestamp(uint32_t timestamp, uint32_t clockRate) {
	return double(timestamp) / double(clockRate);
}

double RtpPacketizationConfig::timestampToSeconds(uint32_t timestamp) {
	return RtpPacketizationConfig::getSecondsFromTimestamp(timestamp, clockRate);
}

uint32_t RtpPacketizationConfig::getTimestampFromSeconds(double seconds, uint32_t clockRate) {
	return uint32_t(int64_t(round(seconds * double(clockRate)))); // convert to integer then cast to u32
}

uint32_t RtpPacketizationConfig::secondsToTimestamp(double seconds) {
	return RtpPacketizationConfig::getTimestampFromSeconds(seconds, clockRate);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
