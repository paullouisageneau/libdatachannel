/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp9rtpdepacketizer.hpp"
#include "rtp.hpp"

namespace rtc {

VP9RtpDepacketizer::VP9RtpDepacketizer() {}

VP9RtpDepacketizer::~VP9RtpDepacketizer() {}

message_ptr VP9RtpDepacketizer::reassemble(message_buffer &buffer) {
	/*
	 * VP9 RTP payload format (RFC 9628)
	 * See https://datatracker.ietf.org/doc/html/rfc9628
	 *
	 * Payload descriptor:
	 *
	 *       0 1 2 3 4 5 6 7
	 *      +-+-+-+-+-+-+-+-+
	 *      |I|P|L|F|B|E|V|Z| (REQUIRED)
	 *      +-+-+-+-+-+-+-+-+
	 *   I: |M| PICTURE ID  | (RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *   M: | EXTENDED PID  | (RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *   L: | TID |U| SID |D| (CONDITIONALLY RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *      |   TL0PICIDX   | (CONDITIONALLY REQUIRED, only if L=1 and F=0)
	 *      +-+-+-+-+-+-+-+-+
	 * P,F: | P_DIFF    |N  | (CONDITIONALLY REQUIRED, up to 3 times)
	 *      +-+-+-+-+-+-+-+-+
	 *   V: | N_S |Y|G|-|-|-| (CONDITIONALLY RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *   Y: |    WIDTH (16)  | (OPTIONAL, per spatial layer)
	 *      +-+-+-+-+-+-+-+-+
	 *      |    HEIGHT (16) | (OPTIONAL, per spatial layer)
	 *      +-+-+-+-+-+-+-+-+
	 *   G: |     N_G       | (OPTIONAL)
	 *      +-+-+-+-+-+-+-+-+
	 * N_G: | TID |U| R |-|-| (OPTIONAL, per picture group)
	 *      +-+-+-+-+-+-+-+-+
	 *      |   P_DIFF      | (OPTIONAL, R times per picture group)
	 *      +-+-+-+-+-+-+-+-+
	 *
	 * I: PictureID present
	 * P: Inter-picture predicted frame
	 * L: Layer indices present
	 * F: Flexible mode (F=1 implies I=1)
	 * B: Start of a frame
	 * E: End of a frame
	 * V: Scalability Structure (SS) data present
	 * Z: Not a reference frame for upper spatial layers
	 * M: PictureID is 15 bits (extended)
	 */

	// First byte bit masks
	const uint8_t bitI = 0b10000000;
	const uint8_t bitP = 0b01000000;
	const uint8_t bitL = 0b00100000;
	const uint8_t bitF = 0b00010000;
	const uint8_t bitB = 0b00001000;
	//const uint8_t bitE = 0b00000100;
	const uint8_t bitV = 0b00000010;

	// PictureID byte
	const uint8_t bitM = 0b10000000;

	// Reference index byte
	const uint8_t bitN = 0b00000001;

	// Maximum number of reference indices in flexible mode
	const int maxRefPics = 3;

	if (buffer.empty())
		return nullptr;

	auto first = *buffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();
	uint16_t nextSeqNumber = firstRtpHeader->seqNumber();

	binary frame;
	std::vector<std::pair<const std::byte *, size_t>> payloads;
	bool continuousSequence = false;
	for (const auto &packet : buffer) {
		auto rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packet->data());
		if (rtpHeader->seqNumber() < nextSeqNumber) {
			// Skip
			continue;
		}
		if (rtpHeader->seqNumber() > nextSeqNumber) {
			// Missing packet(s)
			continuousSequence = false;
		}
		nextSeqNumber = rtpHeader->seqNumber() + 1;

		auto rtpHeaderSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
		auto paddingSize = 0;
		if (rtpHeader->padding())
			paddingSize = std::to_integer<uint8_t>(packet->back());

		if (packet->size() <= rtpHeaderSize + paddingSize)
			continue; // Empty payload

		const std::byte *payloadData = packet->data() + rtpHeaderSize;
		size_t payloadSize = packet->size() - rtpHeaderSize - paddingSize;

		if (payloadSize < 1)
			continue;

		// Parse VP9 payload descriptor (RFC 9628, Section 4.2)
		size_t descriptorSize = 1;
		uint8_t firstByte = std::to_integer<uint8_t>(payloadData[0]);

		// PictureID (conditional: I=1)
		if (firstByte & bitI) {
			if (payloadSize < descriptorSize + 1)
				continue;
			uint8_t pictureIdByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
			descriptorSize++;
			if (pictureIdByte & bitM) { // 15-bit extended PictureID
				if (payloadSize < descriptorSize + 1)
					continue;
				descriptorSize++;
			}
		}

		// Layer indices (conditional: L=1)
		if (firstByte & bitL) {
			if (payloadSize < descriptorSize + 1)
				continue;
			descriptorSize++; // TID | U | SID | D

			// TL0PICIDX present only in non-flexible mode (F=0)
			if (!(firstByte & bitF)) {
				if (payloadSize < descriptorSize + 1)
					continue;
				descriptorSize++;
			}
		}

		// Reference indices (conditional: P=1 and F=1, flexible mode)
		if ((firstByte & bitP) && (firstByte & bitF)) {
			for (int i = 0; i < maxRefPics; i++) {
				if (payloadSize < descriptorSize + 1)
					break;
				uint8_t refByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
				descriptorSize++;
				if (!(refByte & bitN)) // N=0 means last reference index
					break;
			}
		}

		// Scalability Structure (conditional: V=1)
		if (firstByte & bitV) {
			if (payloadSize < descriptorSize + 1)
				continue;
			uint8_t ssByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
			descriptorSize++;

			int numSpatialLayers = (ssByte >> 5) + 1; // N_S + 1
			bool resolutionPresent = (ssByte >> 4) & 0x01; // Y bit
			bool pgPresent = (ssByte >> 3) & 0x01; // G bit

			// Resolution data: WIDTH (2 bytes) + HEIGHT (2 bytes) per spatial layer
			if (resolutionPresent) {
				size_t resolutionSize = 4 * static_cast<size_t>(numSpatialLayers);
				if (payloadSize < descriptorSize + resolutionSize)
					continue;
				descriptorSize += resolutionSize;
			}

			// Picture Group description
			if (pgPresent) {
				if (payloadSize < descriptorSize + 1)
					continue;
				uint8_t numPictureGroups = std::to_integer<uint8_t>(payloadData[descriptorSize]);
				descriptorSize++;

				bool ssTruncated = false;
				for (int i = 0; i < numPictureGroups; i++) {
					if (payloadSize < descriptorSize + 1) {
						ssTruncated = true;
						break;
					}
					uint8_t pgByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
					descriptorSize++;
					int numRefs = (pgByte >> 2) & 0x03; // R field
					if (payloadSize < descriptorSize + static_cast<size_t>(numRefs)) {
						ssTruncated = true;
						break;
					}
					descriptorSize += numRefs;
				}
				if (ssTruncated)
					continue;
			}
		}

		if (payloadSize < descriptorSize)
			continue;

		payloadData += descriptorSize;
		payloadSize -= descriptorSize;

		// Frame reassembly using B (start of frame) and marker bit
		if ((firstByte & bitB) || rtpHeader->marker()) {
			if (continuousSequence) {
				for (auto [data, size] : payloads)
					frame.insert(frame.end(), data, data + size);

				if (rtpHeader->marker())
					frame.insert(frame.end(), payloadData, payloadData + payloadSize);
			}
			else if ((firstByte & bitB) && rtpHeader->marker()) {
				// Single-packet frame at start of buffer
				frame.insert(frame.end(), payloadData, payloadData + payloadSize);
			}
			payloads.clear();
			continuousSequence = true;
		}

		if (!rtpHeader->marker())
			payloads.push_back(std::make_pair(payloadData, payloadSize));
	}

	if (frame.empty())
		return nullptr;

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
