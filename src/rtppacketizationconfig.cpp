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

#if RTC_ENABLE_MEDIA

#include "rtppacketizationconfig.hpp"

#include <cassert>

namespace rtc {

RtpPacketizationConfig::RtpPacketizationConfig(SSRC ssrc, string cname, uint8_t payloadType,
                                               uint32_t clockRate,
                                               optional<uint16_t> sequenceNumber,
                                               optional<uint32_t> timestamp,
                                               uint8_t videoOrientationId)
    : ssrc(ssrc), cname(cname), payloadType(payloadType), clockRate(clockRate), videoOrientationId(videoOrientationId) {
	assert(clockRate > 0);
	srand((unsigned)time(NULL));
	if (sequenceNumber.has_value()) {
		this->sequenceNumber = sequenceNumber.value();
	} else {
		this->sequenceNumber = rand();
	}
	if (timestamp.has_value()) {
		this->timestamp = timestamp.value();
	} else {
		this->timestamp = rand();
	}
	this->mStartTimestamp = this->timestamp;
}

void RtpPacketizationConfig::setStartTime(double startTime, EpochStart epochStart,
                                          optional<uint32_t> startTimestamp) {
	this->mStartTime = startTime + double(static_cast<uint64_t>(epochStart));
	if (startTimestamp.has_value()) {
		this->mStartTimestamp = startTimestamp.value();
		timestamp = this->startTimestamp;
	} else {
		this->mStartTimestamp = timestamp;
	}
}

double RtpPacketizationConfig::getSecondsFromTimestamp(uint32_t timestamp, uint32_t clockRate) {
	return double(timestamp) / double(clockRate);
}

double RtpPacketizationConfig::timestampToSeconds(uint32_t timestamp) {
	return RtpPacketizationConfig::getSecondsFromTimestamp(timestamp, clockRate);
}

uint32_t RtpPacketizationConfig::getTimestampFromSeconds(double seconds, uint32_t clockRate) {
	return uint32_t(int64_t(seconds * double(clockRate))); // convert to integer then cast to u32
}

uint32_t RtpPacketizationConfig::secondsToTimestamp(double seconds) {
	return RtpPacketizationConfig::getTimestampFromSeconds(seconds, clockRate);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
