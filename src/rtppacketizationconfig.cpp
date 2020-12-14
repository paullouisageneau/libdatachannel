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

#if RTC_ENABLE_MEDIA

#include "rtppacketizationconfig.hpp"

namespace rtc {

RTPPacketizationConfig::RTPPacketizationConfig(SSRC ssrc,
                                               string cname,
                                               uint8_t payloadType,
                                               uint32_t clockRate,
                                               std::optional<uint16_t> sequenceNumber,
                                               std::optional<uint32_t> timestamp): ssrc(ssrc), cname(cname), payloadType(payloadType), clockRate(clockRate) {
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
    this->_startTimestamp = this->timestamp;
}

void RTPPacketizationConfig::setStartTime(double startTime_s, EpochStart epochStart, std::optional<uint32_t> startTimestamp) {
    this->_startTime_s = startTime_s + static_cast<unsigned long long>(epochStart);
    if (startTimestamp.has_value()) {
        this->_startTimestamp = startTimestamp.value();
        timestamp = this->startTimestamp;
    } else {
        this->_startTimestamp = timestamp;
    }
}

double RTPPacketizationConfig::getSecondsFromTimestamp(uint32_t timestamp, uint32_t clockRate) {
    return double(timestamp) / double(clockRate);
}

double RTPPacketizationConfig::timestampToSeconds(uint32_t timestamp) {
    return RTPPacketizationConfig::getSecondsFromTimestamp(timestamp, clockRate);
}

uint32_t RTPPacketizationConfig::getTimestampFromSeconds(double seconds, uint32_t clockRate) {
    return uint32_t(seconds * clockRate);
}

uint32_t RTPPacketizationConfig::secondsToTimestamp(double seconds) {
    return RTPPacketizationConfig::getTimestampFromSeconds(seconds, clockRate);
}

} // namespace

#endif /* RTC_ENABLE_MEDIA */
