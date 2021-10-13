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

#include "rtppacketizer.hpp"

#include <cstring>

namespace rtc {

RtpPacketizer::RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig) : rtpConfig(rtpConfig) {}

binary_ptr RtpPacketizer::packetize(shared_ptr<binary> payload, bool setMark) {
	int rtpExtHeaderSize = 0;
	const bool setVideoRotation =
		(rtpConfig->videoOrientationId != 0) &&
		(rtpConfig->videoOrientationId < 15) &&  // needs fixing if longer extension headers are supported
		setMark &&
		(rtpConfig->videoOrientation != 0);
	if (setVideoRotation) {
		rtpExtHeaderSize = rtpExtHeaderCvoSize;
	}
	auto msg = std::make_shared<binary>(rtpHeaderSize + rtpExtHeaderSize + payload->size());
	auto *rtp = (RTP *)msg->data();
	rtp->setPayloadType(rtpConfig->payloadType);
	// increase sequence number
	rtp->setSeqNumber(rtpConfig->sequenceNumber++);
	rtp->setTimestamp(rtpConfig->timestamp);
	rtp->setSsrc(rtpConfig->ssrc);
	if (setMark) {
		rtp->setMarker(true);
	}
	if (rtpExtHeaderSize) {
		rtp->setExtension(true);

		auto extHeader = rtp->getExtensionHeader();
		extHeader->setProfileSpecificId(0xbede);
		extHeader->setHeaderLength(1);
		extHeader->clearBody();
		extHeader->writeCurrentVideoOrientation(0,
			rtpConfig->videoOrientationId, rtpConfig->videoOrientation);
	}
	rtp->preparePacket();
	std::memcpy(msg->data() + rtpHeaderSize + rtpExtHeaderSize, payload->data(), payload->size());
	return msg;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
