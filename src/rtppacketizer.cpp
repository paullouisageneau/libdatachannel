/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtppacketizer.hpp"

#include <cmath>
#include <cstring>

namespace rtc {

RtpPacketizer::RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig) : rtpConfig(rtpConfig) {}

RtpPacketizer::~RtpPacketizer() {}

message_ptr RtpPacketizer::packetize(shared_ptr<binary> payload, bool mark) {
	size_t rtpExtHeaderSize = 0;

	const bool setVideoRotation = (rtpConfig->videoOrientationId != 0) &&
	                              (rtpConfig->videoOrientationId <
	                               15) && // needs fixing if longer extension headers are supported
	                              mark &&
	                              (rtpConfig->videoOrientation != 0);

	if (setVideoRotation)
		rtpExtHeaderSize += 2;

	if (rtpConfig->mid.has_value())
		rtpExtHeaderSize += (1 + rtpConfig->mid->length());

	if (rtpConfig->rid.has_value())
		rtpExtHeaderSize += (1 + rtpConfig->rid->length());

	if (rtpExtHeaderSize != 0)
		rtpExtHeaderSize += 4;

	auto message = make_message(RtpHeaderSize + rtpExtHeaderSize + payload->size());
	auto *rtp = (RtpHeader *)message->data();
	rtp->setPayloadType(rtpConfig->payloadType);
	rtp->setSeqNumber(rtpConfig->sequenceNumber++); // increase sequence number
	rtp->setTimestamp(rtpConfig->timestamp);
	rtp->setSsrc(rtpConfig->ssrc);

	if (mark) {
		rtp->setMarker(true);
	}

	if (rtpExtHeaderSize) {
		rtp->setExtension(true);

		auto extHeader = rtp->getExtensionHeader();
		extHeader->setProfileSpecificId(0xbede);

		auto headerLength = static_cast<uint16_t>(rtpExtHeaderSize - 4);
		headerLength = static_cast<uint16_t>((headerLength + 3) / 4);

		extHeader->setHeaderLength(headerLength);
		extHeader->clearBody();

		size_t offset = 0;
		if (setVideoRotation) {
			extHeader->writeCurrentVideoOrientation(offset, rtpConfig->videoOrientationId,
			                                        rtpConfig->videoOrientation);
			offset += 2;
		}

		if (rtpConfig->mid.has_value()) {
			extHeader->writeOneByteHeader(
			    offset, rtpConfig->midId,
			    reinterpret_cast<const std::byte *>(rtpConfig->mid->c_str()),
			    rtpConfig->mid->length());
			offset += (1 + rtpConfig->mid->length());
		}

		if (rtpConfig->rid.has_value()) {
			extHeader->writeOneByteHeader(
			    offset, rtpConfig->ridId,
			    reinterpret_cast<const std::byte *>(rtpConfig->rid->c_str()),
			    rtpConfig->rid->length());
		}
	}

	rtp->preparePacket();

	std::memcpy(message->data() + RtpHeaderSize + rtpExtHeaderSize, payload->data(),
	            payload->size());

	return message;
}

void RtpPacketizer::media([[maybe_unused]] const Description::Media &desc) {}

void RtpPacketizer::outgoing([[maybe_unused]] message_vector &messages,
                             [[maybe_unused]] const message_callback &send) {
	// Default implementation
	for (auto &message : messages)
		message = packetize(message, false);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
