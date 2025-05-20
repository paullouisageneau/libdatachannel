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

std::vector<binary> RtpPacketizer::fragment(binary data) {
	// Default implementation
	return {std::move(data)};
}

message_ptr RtpPacketizer::packetize(const binary &payload, bool mark) {
	size_t rtpExtHeaderSize = 0;
	bool twoByteHeader = false;

	const bool setVideoRotation =
	    (rtpConfig->videoOrientationId != 0) && mark && (rtpConfig->videoOrientation != 0);

	std::optional<DependencyDescriptorWriter> ddWriter;
	if (rtpConfig->dependencyDescriptorContext.has_value()) {
		ddWriter.emplace(*rtpConfig->dependencyDescriptorContext);
	}

	// Determine if a two-byte header is necessary
	// Check for dependency descriptor extension
	if (ddWriter.has_value()) {
		auto sizeBytes = ddWriter->getSize();
		if (sizeBytes > 16 || rtpConfig->dependencyDescriptorId > 14) {
			twoByteHeader = true;
		}
	}
	// Check for other extensions
	if ((setVideoRotation && rtpConfig->videoOrientationId > 14) ||
	    (rtpConfig->mid.has_value() && rtpConfig->midId > 14) ||
	    (rtpConfig->rid.has_value() && rtpConfig->ridId > 14) ||
	    rtpConfig->playoutDelayId > 14) {
		twoByteHeader = true;
	}
	size_t headerSize = twoByteHeader ? 2 : 1;

	if (setVideoRotation)
		rtpExtHeaderSize += headerSize + 1;

	const bool setPlayoutDelay = rtpConfig->playoutDelayId > 0;

	if (setPlayoutDelay)
		rtpExtHeaderSize += headerSize + 3;

	if (rtpConfig->mid.has_value())
		rtpExtHeaderSize += headerSize + rtpConfig->mid->length();

	if (rtpConfig->rid.has_value())
		rtpExtHeaderSize += headerSize + rtpConfig->rid->length();

	if (ddWriter.has_value()) {
		rtpExtHeaderSize += headerSize + ddWriter->getSize();
	}

	if (rtpExtHeaderSize != 0)
		rtpExtHeaderSize += 4;

	rtpExtHeaderSize = (rtpExtHeaderSize + 3) & ~3;

	auto message = make_message(RtpHeaderSize + rtpExtHeaderSize + payload.size());
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
		extHeader->setProfileSpecificId(twoByteHeader ? 0x1000 : 0xbede);

		auto headerLength = static_cast<uint16_t>(rtpExtHeaderSize / 4) - 1;

		extHeader->setHeaderLength(headerLength);
		extHeader->clearBody();

		size_t offset = 0;
		if (setVideoRotation) {
			offset += extHeader->writeCurrentVideoOrientation(
			    twoByteHeader, offset, rtpConfig->videoOrientationId, rtpConfig->videoOrientation);
		}

		if (rtpConfig->mid.has_value()) {
			offset +=
			    extHeader->writeHeader(twoByteHeader, offset, rtpConfig->midId,
			                           reinterpret_cast<const std::byte *>(rtpConfig->mid->c_str()),
			                           rtpConfig->mid->length());
		}

		if (rtpConfig->rid.has_value()) {
			offset +=
			    extHeader->writeHeader(twoByteHeader, offset, rtpConfig->ridId,
			                           reinterpret_cast<const std::byte *>(rtpConfig->rid->c_str()),
			                           rtpConfig->rid->length());
		}

		if (ddWriter.has_value()) {
			auto sizeBytes = ddWriter->getSize();
			std::vector<std::byte> buf(sizeBytes);
			ddWriter->writeTo(buf.data(), sizeBytes);
			offset += extHeader->writeHeader(
			    twoByteHeader, offset, rtpConfig->dependencyDescriptorId, buf.data(), sizeBytes);
		}

		if (setPlayoutDelay) {
			uint16_t min = rtpConfig->playoutDelayMin & 0xFFF;
			uint16_t max = rtpConfig->playoutDelayMax & 0xFFF;

			// 12 bits for min + 12 bits for max
			byte data[] = {byte((min >> 4) & 0xFF), byte(((min & 0xF) << 4) | ((max >> 8) & 0xF)),
			               byte(max & 0xFF)};

			offset += extHeader->writeHeader(
			    twoByteHeader, offset, rtpConfig->playoutDelayId, data, 3);
		}
	}

	rtp->preparePacket();

	std::memcpy(message->data() + RtpHeaderSize + rtpExtHeaderSize, payload.data(), payload.size());

	return message;
}

message_ptr RtpPacketizer::packetize(shared_ptr<binary> payload, bool mark) {
	return packetize(*payload, mark);
}

void RtpPacketizer::media([[maybe_unused]] const Description::Media &desc) {}

void RtpPacketizer::outgoing(message_vector &messages,
                             [[maybe_unused]] const message_callback &send) {
	message_vector result;
	for (const auto &message : messages) {
		if (const auto &frameInfo = message->frameInfo) {
			if (frameInfo->payloadType && frameInfo->payloadType != rtpConfig->payloadType)
				continue;

			if (frameInfo->timestampSeconds)
				rtpConfig->timestamp =
				    rtpConfig->startTimestamp +
				    rtpConfig->secondsToTimestamp(
				        std::chrono::duration<double>(*frameInfo->timestampSeconds).count());
			else
				rtpConfig->timestamp = frameInfo->timestamp;
		}

		auto payloads = fragment(std::move(*message));
		for (size_t i = 0; i < payloads.size(); i++) {
			if (rtpConfig->dependencyDescriptorContext.has_value()) {
				auto &ctx = *rtpConfig->dependencyDescriptorContext;
				ctx.descriptor.startOfFrame = i == 0;
				ctx.descriptor.endOfFrame = i == payloads.size() - 1;
			}
			bool mark = i == payloads.size() - 1;
			result.push_back(packetize(payloads[i], mark));
		}
	}

	messages.swap(result);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
