/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp8rtpdepacketizer.hpp"
#include "rtp.hpp"

namespace rtc {

VP8RtpDepacketizer::VP8RtpDepacketizer() {}

VP8RtpDepacketizer::~VP8RtpDepacketizer() {}

message_ptr VP8RtpDepacketizer::reassemble(message_buffer &buffer) {
	/*
	 * Implements the recommended partition reconstruction algorithm in RFC 7741
	 * See https://datatracker.ietf.org/doc/html/rfc7741#section-4.5.2
	 *
	 * VP8 payload descriptor (RFC 7741)
	 * See https://www.rfc-editor.org/rfc/rfc7741.html#section-4.2
	 *
	 *      0 1 2 3 4 5 6 7
	 *     +-+-+-+-+-+-+-+-+
	 *     |X|R|N|S|R| PID | (REQUIRED)
	 *     +-+-+-+-+-+-+-+-+
	 *  X: |I|L|T|K| RSV   | (OPTIONAL)
	 *     +-+-+-+-+-+-+-+-+
	 *  I: |M| PictureID   | (OPTIONAL)
	 *     +-+-+-+-+-+-+-+-+
	 *  L: |   TL0PICIDX   | (OPTIONAL)
	 *     +-+-+-+-+-+-+-+-+
	 * T/K:|TID|Y| KEYIDX  | (OPTIONAL)
	 *     +-+-+-+-+-+-+-+-+
	 *
	 * X: Extended control bits present
	 * R: Reserved (MUST be set to 0 and ignored by receiver)
	 * N: Non-reference frame
	 * S: Start of VP8 partition (1 for first fragment, 0 otherwise)
	 * PID: Partition index
	 * I: PictureID present
	 * L: TL0PICIDX present
	 * T: TID present
	 * K: KEYIDX present
	 * RSV: Reserved (MUST be set to 0 and ignored by receiver)
	 * M: PictureID 15-bit extension flag
	 */

	// First byte
	const uint8_t X = 0b10000000;
	//const uint8_t N = 0b00100000;
	const uint8_t S = 0b00010000;

	// Extension byte
	const uint8_t I = 0b10000000;
	const uint8_t L = 0b01000000;
	const uint8_t T = 0b00100000;
	const uint8_t K = 0b00010000;

	// PictureID byte
	const uint8_t M = 0b10000000;

	if (buffer.empty())
		return nullptr;

	auto first = *buffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();
	uint16_t nextSeqNumber = firstRtpHeader->seqNumber();

	binary frame;
	std::vector<std::pair<const std::byte*, size_t>> payloads;
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

		size_t descriptorSize = 1;
		uint8_t firstByte = std::to_integer<uint8_t>(payloadData[0]);

		if (firstByte & X) {
			if (payloadSize < descriptorSize + 1)
				continue;

			uint8_t extensionByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
			descriptorSize++;

			if (extensionByte & I) {
				if (payloadSize < descriptorSize + 1)
					continue;
				uint8_t pictureIdByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
				descriptorSize++;
				if (pictureIdByte & M) { // M bit, 15-bit PictureID
					if (payloadSize < descriptorSize + 1)
						continue;
					descriptorSize++;
				}
			}

			if (extensionByte & L) {
				if (payloadSize < descriptorSize + 1)
					continue;
				descriptorSize++;
			}

			if ((extensionByte & T) || (extensionByte & K)) {
				if (payloadSize < descriptorSize + 1)
					continue;
				descriptorSize++;
			}
		}

		if (payloadSize < descriptorSize)
			continue;

		payloadData += descriptorSize;
		payloadSize -= descriptorSize;

		if (firstByte & S || rtpHeader->marker()) {
			if (continuousSequence) {
				// Sequence is continuous, append to frame
				for (auto [data, size] : payloads)
					frame.insert(frame.end(), data, data + size);

				if (rtpHeader->marker()) {
					// Add current payload too
					frame.insert(frame.end(), payloadData, payloadData + payloadSize);
				}
			}
			payloads.clear();
			continuousSequence = true;
		}

		if (!rtpHeader->marker())
			payloads.push_back(std::make_pair(payloadData, payloadSize));
	}

	if(frame.empty()) {
		// No partition was recoverable
		return nullptr;
	}

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
