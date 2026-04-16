/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp9rtppacketizer.hpp"

#include <cstring>

namespace rtc {

VP9RtpPacketizer::VP9RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
		size_t maxFragmentSize)
	: RtpPacketizer(std::move(rtpConfig)), mMaxFragmentSize(maxFragmentSize) {}

std::vector<binary> VP9RtpPacketizer::fragment(binary frame) {
	/*
	 * VP9 RTP payload descriptor (RFC 9628)
	 * See https://datatracker.ietf.org/doc/html/rfc9628
	 *
	 *       0 1 2 3 4 5 6 7
	 *      +-+-+-+-+-+-+-+-+
	 *      |I|P|L|F|B|E|V|Z| (REQUIRED)
	 *      +-+-+-+-+-+-+-+-+
	 *
	 * I: PictureID present
	 * P: Inter-picture predicted frame
	 * L: Layer indices present
	 * F: Flexible mode
	 * B: Start of a frame
	 * E: End of a frame
	 * V: Scalability Structure (SS) data present
	 * Z: Not a reference frame for upper spatial layers
	 *
	 * We use a minimal 1-byte descriptor with only B and E flags set.
	 * This is sufficient for single spatial/temporal layer streams.
	 */

	// First byte bit masks
	const uint8_t bitP = 0b01000000;
	const uint8_t bitB = 0b00001000;
	const uint8_t bitE = 0b00000100;

	/*
	 * VP9 uncompressed header (VP9 bitstream spec, Section 6.2):
	 * The first two bits of the first byte encode the frame marker (0b10),
	 * and the profile. Bit 3 (0x08) is the show_existing_frame flag.
	 * When show_existing_frame is 0, bit 2 (0x04) is 0 for a key frame
	 * and 1 for a non-key frame.
	 *
	 * However, the exact bit-level positions depend on the profile and
	 * other variable-length fields. A simpler heuristic used widely:
	 * a VP9 key frame starts with byte value 0x82 or 0x83 (frame marker
	 * 0b10 + profile bits), but this is fragile.
	 *
	 * The robust check: bit 3 (show_existing_frame) must be 0, and
	 * bit 2 is the frame_type: 0 = KEY_FRAME, 1 = NON_KEY_FRAME.
	 */
	const uint8_t showExistingFrame = 0b00001000; // bit 3 (0x08)
	const uint8_t frameTypeBit = 0b00000100;     // bit 2 (0x04)

	if (frame.empty())
		return {};

	bool isInterFrame = false;
	uint8_t firstFrameByte = std::to_integer<uint8_t>(frame[0]);
	if (!(firstFrameByte & showExistingFrame))
		isInterFrame = (firstFrameByte & frameTypeBit) != 0;

	const size_t descriptorSize = 1;
	if (mMaxFragmentSize <= descriptorSize)
		return {};

	std::vector<binary> payloads;
	size_t index = 0;
	while (index < frame.size()) {
		size_t remaining = frame.size() - index;
		size_t payloadSize = std::min(mMaxFragmentSize - descriptorSize, remaining);
		bool isFirst = (index == 0);
		bool isLast = (index + payloadSize >= frame.size());

		binary payload(descriptorSize + payloadSize);

		// Set 1-byte payload descriptor
		uint8_t descriptor = 0;
		if (isInterFrame)
			descriptor |= bitP;
		if (isFirst)
			descriptor |= bitB;
		if (isLast)
			descriptor |= bitE;
		payload[0] = std::byte(descriptor);

		// Copy frame data
		std::memcpy(payload.data() + descriptorSize, frame.data() + index, payloadSize);

		payloads.push_back(std::move(payload));
		index += payloadSize;
	}

	return payloads;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
