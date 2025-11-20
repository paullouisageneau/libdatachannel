/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp8rtppacketizer.hpp"

#include <cstring>

namespace rtc {

VP8RtpPacketizer::VP8RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
		size_t maxFragmentSize)
	: RtpPacketizer(std::move(rtpConfig)), mMaxFragmentSize(maxFragmentSize) {}

std::vector<binary> VP8RtpPacketizer::fragment(binary frame) {
	/*
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
	 * R: Reserved (MUST be set to 0)
	 * N: Non-reference frame
	 * S: Start of VP8 partition (1 for first fragment, 0 otherwise)
	 * PID: Partition index
	 * I: PictureID present
	 * L: TL0PICIDX present
	 * T: TID present
	 * K: KEYIDX present
	 * M: PictureID 15-bit extension flag
	 */

	// First descriptor byte
	const uint8_t N = 0b00100000;
	const uint8_t S = 0b00010000;

	/*
	 * The beginning of an encoded VP8 frame is referred to as an "uncompressed data chunk"
	 * in RFC6386 and co-serve as payload header in this RTP format. The codec bitstream
	 * format specifies two different variants of the uncompressed data chunk: a 3 octet
	 * version for interframes and a 10 octet version for key frames. The first 3 octets
	 * are common to both variants.
	 * See https://datatracker.ietf.org/doc/html/draft-ietf-payload-vp8-08#section-4.3
	 *
	 *  0 1 2 3 4 5 6 7
	 * +-+-+-+-+-+-+-+-+
	 * |Size0|H| VER |P|
	 * +-+-+-+-+-+-+-+-+
	 * |     Size1     |
	 * +-+-+-+-+-+-+-+-+
	 * |     Size2     |
	 * +-+-+-+-+-+-+-+-+
	 *
	 * H: Show frame bit as defined in RFC6386.
	 * VER: A version number as defined in RFC6386.
	 * P: Inverse key frame flag.  When set to 0 the current frame is a key frame.
	 *    When set to 1 the current frame is an interframe.
	 * SizeN: The size of the first partition size in bytes is calculated
	 *        from the 19 bits in Size0, Size1, and Size2 as 1stPartitionSize =
	 *        Size0 + 8 * Size1 + 2048 * Size2.
	 */

	// First frame byte
	const uint8_t P = 0b00000001;

	if (frame.size() < 3)
		return {};

	const bool isKeyframe = (std::to_integer<uint8_t>(frame[0]) & P) == 0;

	const size_t descriptorSize = 1;
	if (mMaxFragmentSize <= descriptorSize)
		return {};

	std::vector<binary> payloads;
	size_t index = 0;
	while (index < frame.size()) {
		size_t remaining = frame.size() - index;
		size_t payloadSize = std::min(mMaxFragmentSize - descriptorSize, remaining);

		binary payload(descriptorSize + payloadSize);

		// Set 1-byte payload descriptor
		uint8_t descriptor = 0;
		if (!isKeyframe)
			descriptor |= N;
		if (index == 0)
			descriptor |= S;
		payload[0] = std::byte(descriptor);

		// Copy data
		std::memcpy(payload.data() + descriptorSize, frame.data() + index, payloadSize);

		payloads.push_back(std::move(payload));
		index += payloadSize;
	}

	return payloads;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
