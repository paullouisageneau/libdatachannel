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

#include "rtppacketizer.hpp"

namespace rtc {

RtpPacketizer::RtpPacketizer(std::shared_ptr<RtpPacketizationConfig> rtpConfig)
    : rtpConfig(rtpConfig) {}

binary_ptr RtpPacketizer::packetize(std::shared_ptr<binary> payload, bool setMark) {
	auto msg = std::make_shared<binary>(rtpHeaderSize + payload->size());
	auto *rtp = (RTP *)msg->data();
	rtp->setPayloadType(rtpConfig->payloadType);
	// increase sequence number
	rtp->setSeqNumber(rtpConfig->sequenceNumber++);
	rtp->setTimestamp(rtpConfig->timestamp);
	rtp->setSsrc(rtpConfig->ssrc);
	if (setMark) {
		rtp->setMarker(true);
	}
	rtp->preparePacket();
	memcpy(msg->data() + rtpHeaderSize, payload->data(), payload->size());
	return msg;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
