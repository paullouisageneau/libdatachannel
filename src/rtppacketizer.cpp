/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
	auto *rtp = (RtpHeader *)msg->data();
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
