/**
 * Copyright (c) 2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "av1rtpdepacketizer.hpp"
#include "rtp.hpp"

namespace rtc {

namespace {

void appendObuWithSize(binary &frame, const std::byte *data, size_t size) {
	if (size < 1)
		return;

	uint8_t header = std::to_integer<uint8_t>(data[0]);
	bool hasExtension = (header >> 2) & 1;
	bool hasSizeField = (header >> 1) & 1;
	size_t headerSize = 1 + (hasExtension ? 1 : 0);

	if (size < headerSize)
		return;

	const std::byte *payloadData;
	size_t payloadSize;

	if (hasSizeField) {
		size_t offset = headerSize;
		uint32_t existingSize = 0;
		for (int i = 0; i < 4 && offset < size; i++) {
			uint8_t b = std::to_integer<uint8_t>(data[offset++]);
			existingSize |= uint32_t(b & 0x7F) << (i * 7);
			if (!(b & 0x80))
				break;
		}
		payloadData = data + offset;
		payloadSize = size - offset;
		if (existingSize < payloadSize)
			payloadSize = existingSize;
	} else {
		payloadData = data + headerSize;
		payloadSize = size - headerSize;
	}

	frame.push_back(std::byte(header | 0x02));

	if (hasExtension)
		frame.push_back(data[1]);

	uint32_t remaining = static_cast<uint32_t>(payloadSize);
	do {
		uint8_t b = remaining & 0x7F;
		remaining >>= 7;
		if (remaining > 0)
			b |= 0x80;
		frame.push_back(std::byte(b));
	} while (remaining > 0);

	frame.insert(frame.end(), payloadData, payloadData + payloadSize);
}

} // anonymous namespace

AV1RtpDepacketizer::AV1RtpDepacketizer(Packetization packetization)
    : mPacketization(packetization) {}

AV1RtpDepacketizer::~AV1RtpDepacketizer() {}

message_ptr AV1RtpDepacketizer::reassemble(message_buffer &buffer) {
	/*
	 * AV1 RTP aggregation header (https://aomediacodec.github.io/av1-rtp-spec/)
	 *
	 *  0 1 2 3 4 5 6 7
	 * +-+-+-+-+-+-+-+-+
	 * |Z|Y| W |N|-|-|-|
	 * +-+-+-+-+-+-+-+-+
	 *
	 * Z: first OBU element is a continuation fragment from the previous packet
	 * Y: last OBU element continues in the next packet
	 * W: number of OBU elements (0 = variable, each with length prefix;
	 *    1-3 = exact count, all but last have length prefix)
	 * N: first packet of a coded video sequence
	 */

	const uint8_t zMask = 0b10000000;
	const uint8_t yMask = 0b01000000;
	const uint8_t wMask = 0b00110000;
	const uint8_t wBitshift = 4;

	const uint8_t sevenLsbBitmask = 0b01111111;
	const uint8_t msbBitmask = 0b10000000;

	if (buffer.empty())
		return nullptr;

	auto first = *buffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();

	binary frame;
	binary pendingFragment;

	for (const auto &packet : buffer) {
		auto rtpHeader = reinterpret_cast<const RtpHeader *>(packet->data());
		auto rtpHeaderSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
		auto paddingSize = 0;
		if (rtpHeader->padding())
			paddingSize = std::to_integer<uint8_t>(packet->back());

		if (packet->size() <= rtpHeaderSize + paddingSize + 1)
			continue;

		const std::byte *payload = packet->data() + rtpHeaderSize;
		size_t payloadSize = packet->size() - rtpHeaderSize - paddingSize;

		if (payloadSize < 1)
			continue;

		uint8_t aggHeader = std::to_integer<uint8_t>(payload[0]);
		bool zBit = (aggHeader & zMask) != 0;
		bool yBit = (aggHeader & yMask) != 0;
		uint8_t w = (aggHeader & wMask) >> wBitshift;

		size_t offset = 1;

		// Extract OBU elements
		std::vector<std::pair<const std::byte *, size_t>> elements;

		auto parseLeb128 = [&](uint32_t &out) -> bool {
			out = 0;
			for (int i = 0; i < 4 && offset < payloadSize; i++) {
				uint8_t b = std::to_integer<uint8_t>(payload[offset++]);
				out |= uint32_t(b & sevenLsbBitmask) << (i * 7);
				if (!(b & msbBitmask))
					return true;
			}
			return false;
		};

		if (w == 0) {
			// All elements have a LEB128 length prefix
			while (offset < payloadSize) {
				uint32_t elemLen = 0;
				if (!parseLeb128(elemLen))
					break;
				if (offset + elemLen > payloadSize)
					break;
				elements.emplace_back(payload + offset, elemLen);
				offset += elemLen;
			}
		} else {
			// W elements: first W-1 have LEB128 length prefix, last uses remaining bytes
			for (uint8_t i = 0; i < w && offset <= payloadSize; i++) {
				if (i < w - 1) {
					uint32_t elemLen = 0;
					if (!parseLeb128(elemLen))
						break;
					if (offset + elemLen > payloadSize)
						break;
					elements.emplace_back(payload + offset, elemLen);
					offset += elemLen;
				} else {
					elements.emplace_back(payload + offset, payloadSize - offset);
				}
			}
		}

		// Reassemble OBU elements, handling Z/Y fragmentation
		size_t n = elements.size();
		for (size_t i = 0; i < n; i++) {
			auto [data, size] = elements[i];
			bool isFirst = (i == 0);
			bool isLast = (i == n - 1);

			if (isFirst && zBit) {
				// Continuation of pending OBU fragment
				pendingFragment.insert(pendingFragment.end(), data, data + size);
				if (!(isLast && yBit)) {
					// Fragment complete
					if (mPacketization == Packetization::TemporalUnit)
						appendObuWithSize(frame, pendingFragment.data(), pendingFragment.size());
					else
						frame.insert(frame.end(), pendingFragment.begin(), pendingFragment.end());
					pendingFragment.clear();
				}
			} else if (isLast && yBit) {
				// This element continues in the next packet
				pendingFragment.insert(pendingFragment.end(), data, data + size);
			} else {
				if (mPacketization == Packetization::TemporalUnit)
					appendObuWithSize(frame, data, size);
				else
					frame.insert(frame.end(), data, data + size);
			}
		}
	}

	if (frame.empty())
		return nullptr;

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
