/**
 * Copyright (c) 2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "h264rtpdepacketizer.hpp"
#include "nalunit.hpp"

#include "impl/internals.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>

namespace rtc {

const binary naluLongStartCode = {byte{0}, byte{0}, byte{0}, byte{1}};
const binary naluShortStartCode = {byte{0}, byte{0}, byte{1}};

const uint8_t naluTypeSTAPA = 24;
const uint8_t naluTypeFUA = 28;

bool H264RtpDepacketizer::sequence_cmp::operator()(message_ptr a, message_ptr b) const {
	assert(a->size() >= sizeof(RtpHeader) && b->size() >= sizeof(RtpHeader));
	auto ha = reinterpret_cast<const rtc::RtpHeader *>(a->data());
	auto hb = reinterpret_cast<const rtc::RtpHeader *>(b->data());
	int16_t d = int16_t(hb->seqNumber() - ha->seqNumber());
	return d > 0;
}

H264RtpDepacketizer::H264RtpDepacketizer(Separator separator) : mSeparator(separator) {
	if (separator != Separator::StartSequence && separator != Separator::LongStartSequence &&
	    separator != Separator::ShortStartSequence) {
		throw std::invalid_argument("Invalid separator");
	}
}

void H264RtpDepacketizer::addSeparator(binary &accessUnit) {
	if (mSeparator == Separator::StartSequence || mSeparator == Separator::LongStartSequence) {
		accessUnit.insert(accessUnit.end(), naluLongStartCode.begin(), naluLongStartCode.end());
	} else if (mSeparator == Separator::ShortStartSequence) {
		accessUnit.insert(accessUnit.end(), naluShortStartCode.begin(), naluShortStartCode.end());
	} else {
		throw std::invalid_argument("Invalid separator");
	}
}

message_ptr H264RtpDepacketizer::buildFrame() {
	if (mBuffer.empty())
		return nullptr;

	auto first = *mBuffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();
	uint16_t nextSeqNumber = firstRtpHeader->seqNumber();

	binary frame;
	bool continuousFragments = false;
	for (const auto &packet : mBuffer) {
		auto rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packet->data());
		if (rtpHeader->seqNumber() < nextSeqNumber) {
			// Skip
			continue; // skip
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

		auto nalUnitHeader = NalUnitHeader{std::to_integer<uint8_t>(packet->at(rtpHeaderSize))};

		if (nalUnitHeader.unitType() == naluTypeFUA) {
			if (packet->size() <= rtpHeaderSize + paddingSize + 1)
				continue; // Empty FU-A

			auto nalUnitFragmentHeader =
			    NalUnitFragmentHeader{std::to_integer<uint8_t>(packet->at(rtpHeaderSize + 1))};

			// RFC 6184: When set to one, the Start bit indicates the start of a fragmented NAL
			// unit. When the following FU payload is not the start of a fragmented NAL unit
			// payload, the Start bit is set to zero.
			if (nalUnitFragmentHeader.isStart()) {
				addSeparator(frame);
				frame.emplace_back(byte(nalUnitHeader.idc() | nalUnitFragmentHeader.unitType()));
				continuousFragments = true;
			}

			// RFC 6184: If a fragmentation unit is lost, the receiver SHOULD discard all following
			// fragmentation units in transmission order corresponding to the same fragmented NAL
			// unit.
			if (continuousFragments) {
				frame.insert(frame.end(), packet->begin() + rtpHeaderSize + 2,
				             packet->end() - paddingSize);
			}

			// RFC 6184: When set to one, the End bit indicates the end of a fragmented NAL unit,
			// i.e., the last byte of the payload is also the last byte of the fragmented NAL unit.
			// When the following FU payload is not the last fragment of a fragmented NAL unit, the
			// End bit is set to zero.
			if (nalUnitFragmentHeader.isEnd())
				continuousFragments = false;

		} else {
			continuousFragments = false;

			if (nalUnitHeader.unitType() == naluTypeSTAPA) {
				auto offset = rtpHeaderSize + 1;

				while (offset + 2 < packet->size() - paddingSize) {
					auto naluSize = std::to_integer<uint16_t>(packet->at(offset)) << 8 |
					                std::to_integer<uint16_t>(packet->at(offset + 1));

					offset += 2;

					if (offset + naluSize > packet->size() - paddingSize)
						throw std::runtime_error("H264 STAP-A size is larger than payload");

					addSeparator(frame);
					frame.insert(frame.end(), packet->begin() + offset,
					             packet->begin() + offset + naluSize);

					offset += naluSize;
				}

			} else if (nalUnitHeader.unitType() > 0 && nalUnitHeader.unitType() < 24) {
				addSeparator(frame);
				frame.insert(frame.end(), packet->begin() + rtpHeaderSize,
				             packet->end() - paddingSize);

			} else {
				throw std::runtime_error("Unknown H264 RTP Packetization");
			}
		}
	}

	auto frameInfo = std::make_shared<FrameInfo>(timestamp);
	frameInfo->timestampSeconds =
	    std::chrono::duration<double>(double(timestamp) / double(ClockRate));
	frameInfo->payloadType = payloadType;
	return make_message(std::move(frame), std::move(frameInfo));
}

void H264RtpDepacketizer::incoming(message_vector &messages, const message_callback &) {
	message_vector result;
	for (auto message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			continue;
		}

		if (message->size() < sizeof(RtpHeader)) {
			PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
			continue;
		}

		auto header = reinterpret_cast<const RtpHeader *>(message->data());

		if (!mBuffer.empty()) {
			auto first = *mBuffer.begin();
			auto firstHeader = reinterpret_cast<const RtpHeader *>(first->data());
			if (firstHeader->timestamp() != header->timestamp()) {
				if (auto frame = buildFrame())
					result.push_back(frame);

				mBuffer.clear();
			}
		}

		mBuffer.insert(std::move(message));

		if (header->marker()) {
			if (auto frame = buildFrame())
				result.push_back(std::move(frame));

			mBuffer.clear();
		}
	};

	messages.swap(result);
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
