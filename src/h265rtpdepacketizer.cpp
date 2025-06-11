/**
 * Copyright (c) 2023-2024 Paul-Louis Ageneau
 * Copyright (c) 2024 Robert Edmonds
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "h265rtpdepacketizer.hpp"
#include "h265nalunit.hpp"

#include "impl/internals.hpp"

#include <algorithm>

namespace rtc {

const binary naluLongStartCode = {byte{0}, byte{0}, byte{0}, byte{1}};
const binary naluShortStartCode = {byte{0}, byte{0}, byte{1}};

const uint8_t naluTypeAP = 48;
const uint8_t naluTypeFU = 49;

H265RtpDepacketizer::H265RtpDepacketizer(Separator separator) : mSeparator(separator) {
	if (separator != Separator::StartSequence && separator != Separator::LongStartSequence &&
	    separator != Separator::ShortStartSequence) {
		throw std::invalid_argument("Unimplemented H265 separator");
	}
}

H265RtpDepacketizer::~H265RtpDepacketizer() {}

message_ptr H265RtpDepacketizer::reassemble(message_buffer &buffer) {
	if (buffer.empty())
		return nullptr;

	auto first = *buffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();
	uint16_t nextSeqNumber = firstRtpHeader->seqNumber();

	binary frame;
	bool continuousFragments = false;
	for (const auto &packet : buffer) {
		auto rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packet->data());
		if (rtpHeader->seqNumber() < nextSeqNumber) {
			// Skip
			continue;
		}
		if (rtpHeader->seqNumber() > nextSeqNumber) {
			// Missing packet(s)
			continuousFragments = false;
		}

		nextSeqNumber = rtpHeader->seqNumber() + 1;

		auto rtpHeaderSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
		auto paddingSize = 0;
		if (rtpHeader->padding())
			paddingSize = std::to_integer<uint8_t>(packet->back());

		if (packet->size() <= rtpHeaderSize + paddingSize)
			continue; // Empty payload

		size_t payloadSize = packet->size() - (rtpHeaderSize + paddingSize);
		if (payloadSize < 2)
			throw std::runtime_error("Truncated H265 NAL unit");

		auto nalUnitHeader =
		    H265NalUnitHeader{std::to_integer<uint8_t>(packet->at(rtpHeaderSize)),
		                      std::to_integer<uint8_t>(packet->at(rtpHeaderSize + 1))};

		if (nalUnitHeader.unitType() == naluTypeFU) {
			if (payloadSize <= 2)
				continue; // Empty FU

			auto nalUnitFragmentHeader =
			    H265NalUnitFragmentHeader{std::to_integer<uint8_t>(packet->at(rtpHeaderSize + 2))};

			// RFC 7798: When set to 1, the S bit indicates the start of a fragmented
			// NAL unit, i.e., the first byte of the FU payload is also the first byte of
			// the payload of the fragmented NAL unit. When the FU payload is not the start
			// of the fragmented NAL unit payload, the S bit MUST be set to 0.
			if (nalUnitFragmentHeader.isStart()) {
				addSeparator(frame);
				nalUnitHeader.setUnitType(nalUnitFragmentHeader.unitType());
				frame.emplace_back(byte(nalUnitHeader._first));
				frame.emplace_back(byte(nalUnitHeader._second));
				continuousFragments = true;
			}

			// RFC 7798: If an FU is lost, the receiver SHOULD discard all following fragmentation
			// units in transmission order corresponding to the same fragmented NAL unit
			if (continuousFragments) {
				frame.insert(frame.end(), packet->begin() + rtpHeaderSize + 3,
				             packet->end() - paddingSize);
			}

			// RFC 7798: When set to 1, the E bit indicates the end of a fragmented NAL unit, i.e.,
			// the last byte of the payload is also the last byte of the fragmented NAL unit.  When
			// the FU payload is not the last fragment of a fragmented NAL unit, the E bit MUST be
			// set to 0.
			if (nalUnitFragmentHeader.isEnd())
				continuousFragments = false;

		} else {
			continuousFragments = false;

			if (nalUnitHeader.unitType() == naluTypeAP) {
				auto offset = rtpHeaderSize + 2;

				while (offset + 2 < packet->size() - paddingSize) {
					auto naluSize = std::to_integer<uint16_t>(packet->at(offset)) << 8 |
					                std::to_integer<uint16_t>(packet->at(offset + 1));

					offset += 2;

					if (offset + naluSize > packet->size() - paddingSize)
						throw std::runtime_error("H265 STAP size is larger than payload");

					addSeparator(frame);
					frame.insert(frame.end(), packet->begin() + offset,
					             packet->begin() + offset + naluSize);

					offset += naluSize;
				}

			} else if (nalUnitHeader.unitType() < 47) {
				// RFC 7798: NAL units with NAL unit type values in the range of 0 to 47, inclusive,
				// may be passed to the decoder.
				addSeparator(frame);
				frame.insert(frame.end(), packet->begin() + rtpHeaderSize,
				             packet->end() - paddingSize);

			} else {
				// RFC 7798: NAL-unit-like structures with NAL unit type values in the range of 48
				// to 63, inclusive, MUST NOT be passed to the decoder.
			}
		}
	}

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

void H265RtpDepacketizer::addSeparator(binary &frame) {
	switch (mSeparator) {
	case Separator::StartSequence:
		[[fallthrough]];
	case Separator::LongStartSequence:
		frame.insert(frame.end(), naluLongStartCode.begin(), naluLongStartCode.end());
		break;
	case Separator::ShortStartSequence:
		frame.insert(frame.end(), naluShortStartCode.begin(), naluShortStartCode.end());
		break;
	default:
		throw std::invalid_argument("Invalid separator");
	}
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
