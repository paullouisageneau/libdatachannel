/**
 * Copyright (c) 2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "av1rtppacketizer.hpp"

#include "impl/internals.hpp"

#include <algorithm>

namespace rtc {

const auto payloadHeaderSize = 1;

const auto zMask = byte(0b10000000);
const auto yMask = byte(0b01000000);
const auto nMask = byte(0b00001000);

const auto wBitshift = 4;

const auto obuFrameTypeMask = byte(0b01111000);
const auto obuFrameTypeBitshift = 3;

const auto obuHeaderSize = 1;
const auto obuHasExtensionMask = byte(0b00000100);
const auto obuHasSizeMask = byte(0b00000010);

const auto obuFrameTypeSequenceHeader = byte(1);

const auto obuTemporalUnitDelimiter = std::vector<byte>{byte(0x12), byte(0x00)};

const auto oneByteLeb128Size = 1;

const uint8_t sevenLsbBitmask = 0b01111111;
const uint8_t msbBitmask = 0b10000000;

AV1RtpPacketizer::AV1RtpPacketizer(Packetization packetization,
                                   shared_ptr<RtpPacketizationConfig> rtpConfig,
                                   size_t maxFragmentSize)
    : RtpPacketizer(rtpConfig), mPacketization(packetization), mMaxFragmentSize(maxFragmentSize) {}

std::vector<binary> AV1RtpPacketizer::extractTemporalUnitObus(const binary &data) {
	std::vector<binary> obus;

	if (data.size() <= 2 || (data.at(0) != obuTemporalUnitDelimiter.at(0)) ||
	    (data.at(1) != obuTemporalUnitDelimiter.at(1))) {
		return {};
	}

	size_t index = 2;
	while (index < data.size()) {
		if ((data.at(index) & obuHasSizeMask) == byte(0)) {
			return obus;
		}

		if ((data.at(index) & obuHasExtensionMask) != byte(0)) {
			index++;
		}

		// https://aomediacodec.github.io/av1-spec/#leb128
		uint32_t obuLength = 0;
		uint8_t leb128Size = 0;
		while (leb128Size < 8) {
			auto leb128Index = index + leb128Size + obuHeaderSize;
			if (data.size() < leb128Index) {
				break;
			}

			auto leb128_byte = uint8_t(data.at(leb128Index));

			obuLength |= ((leb128_byte & sevenLsbBitmask) << (leb128Size * 7));
			leb128Size++;

			if (!(leb128_byte & msbBitmask)) {
				break;
			}
		}

		obus.emplace_back(data.begin() + index,
		                  data.begin() + index + obuHeaderSize + leb128Size + obuLength);

		index += obuHeaderSize + leb128Size + obuLength;
	}

	return obus;
}

std::vector<binary> AV1RtpPacketizer::fragment(binary data) {
	if (mPacketization == AV1RtpPacketizer::Packetization::TemporalUnit) {
		std::vector<binary> result;
		auto obus = extractTemporalUnitObus(data);
		for (auto obu : obus) {
			auto fragments = fragmentObu(obu);
			result.reserve(result.size() + fragments.size());
			for(auto &fragment : fragments)
				fragments.push_back(std::move(fragment));
		}
		return result;
	} else {
		return fragmentObu(data);
	}
}

/*
 *  0 1 2 3 4 5 6 7
 * +-+-+-+-+-+-+-+-+
 * |Z|Y| W |N|-|-|-|
 * +-+-+-+-+-+-+-+-+
 *
 *	Z: MUST be set to 1 if the first OBU element is an
 *	   OBU fragment that is a continuation of an OBU fragment
 *	   from the previous packet, and MUST be set to 0 otherwise.
 *
 *	Y: MUST be set to 1 if the last OBU element is an OBU fragment
 *	   that will continue in the next packet, and MUST be set to 0 otherwise.
 *
 *	W: two bit field that describes the number of OBU elements in the packet.
 *	   This field MUST be set equal to 0 or equal to the number of OBU elements
 *	   contained in the packet. If set to 0, each OBU element MUST be preceded by
 *	   a length field. If not set to 0 (i.e., W = 1, 2 or 3) the last OBU element
 *	   MUST NOT be preceded by a length field. Instead, the length of the last OBU
 *	   element contained in the packet can be calculated as follows:
 *	Length of the last OBU element =
 *	   length of the RTP payload
 *	 - length of aggregation header
 *	 - length of previous OBU elements including length fields
 *
 *	N: MUST be set to 1 if the packet is the first packet of a coded video sequence, and MUST be set
 *     to 0 otherwise.
 *
 * https://aomediacodec.github.io/av1-rtp-spec/#44-av1-aggregation-header
 *
 **/

std::vector<binary> AV1RtpPacketizer::fragmentObu(const binary &data) {
	std::vector<binary> payloads;

	if (data.size() < 1)
		return {};

	// Cache sequence header and packetize with next OBU
	auto frameType = (data.at(0) & obuFrameTypeMask) >> obuFrameTypeBitshift;
	if (frameType == obuFrameTypeSequenceHeader) {
		mSequenceHeader = std::make_unique<binary>(data.begin(), data.end());
		return {};
	}

	size_t index = 0;
	size_t remaining = data.size();
	while (remaining > 0) {
		size_t obuCount = 1;
		size_t metadataSize = payloadHeaderSize;

		if (mSequenceHeader) {
			obuCount++;
			metadataSize += 1 + int(mSequenceHeader->size()); // 1 byte leb128
		}

		binary payload(std::min(size_t(mMaxFragmentSize), remaining + metadataSize));
		size_t payloadOffset = payloadHeaderSize;

		payload.at(0) = byte(obuCount) << wBitshift;

		// Packetize cached SequenceHeader
		if (obuCount == 2) {
			payload.at(0) ^= nMask;
			payload.at(1) = byte(mSequenceHeader->size() & sevenLsbBitmask);
			payloadOffset += oneByteLeb128Size;

			std::memcpy(payload.data() + payloadOffset, mSequenceHeader->data(),
			            mSequenceHeader->size());
			payloadOffset += int(mSequenceHeader->size());

			mSequenceHeader = nullptr;
		}

		// Copy as much of OBU as possible into Payload
		size_t payloadRemaining = payload.size() - payloadOffset;
		std::memcpy(payload.data() + payloadOffset, data.data() + index,
		            payloadRemaining);
		remaining -= payloadRemaining;
		index += payloadRemaining;

		// Does this Fragment contain an OBU that started in a previous payload
		if (payloads.size() > 0) {
			payload.at(0) ^= zMask;
		}

		// This OBU will be continued in next Payload
		if (index < data.size()) {
			payload.at(0) ^= yMask;
		}

		payloads.push_back(std::move(payload));
	}

	return payloads;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
