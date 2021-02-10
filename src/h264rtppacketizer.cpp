/*
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#if RTC_ENABLE_MEDIA

#include "h264rtppacketizer.hpp"

namespace rtc {

using std::make_shared;
using std::shared_ptr;

typedef enum {
	NUSM_noMatch,
	NUSM_firstZero,
	NUSM_secondZero,
	NUSM_thirdZero,
	NUSM_shortMatch,
	NUSM_longMatch
} NalUnitStartSequenceMatch;

NalUnitStartSequenceMatch StartSequenceMatchSucc(NalUnitStartSequenceMatch match, byte _byte,
												 H264RtpPacketizer::Separator separator) {
	assert(separator != H264RtpPacketizer::Separator::Length);
	auto byte = (uint8_t)_byte;
	auto detectShort = separator == H264RtpPacketizer::Separator::ShortStartSequence ||
	separator == H264RtpPacketizer::Separator::StartSequence;
	auto detectLong = separator == H264RtpPacketizer::Separator::LongStartSequence ||
	separator == H264RtpPacketizer::Separator::StartSequence;
	switch (match) {
		case NUSM_noMatch:
			if (byte == 0x00) {
				return NUSM_firstZero;
			}
			break;
		case NUSM_firstZero:
			if (byte == 0x00) {
				return NUSM_secondZero;
			}
			break;
		case NUSM_secondZero:
			if (byte == 0x00 && detectLong) {
				return NUSM_thirdZero;
			} else if (byte == 0x01 && detectShort) {
				return NUSM_shortMatch;
			}
			break;
		case NUSM_thirdZero:
			if (byte == 0x01 && detectLong) {
				return NUSM_longMatch;
			}
			break;
		case NUSM_shortMatch:
			return NUSM_shortMatch;
		case NUSM_longMatch:
			return NUSM_longMatch;
	}
	return NUSM_noMatch;
}

shared_ptr<NalUnits> H264RtpPacketizer::splitMessage(binary_ptr message) {
	auto nalus = make_shared<NalUnits>();
	if (separator == Separator::Length) {
		unsigned long long index = 0;
		while (index < message->size()) {
			assert(index + 4 < message->size());
			if (index + 4 >= message->size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete length), ignoring!";
				break;
			}
			auto lengthPtr = (uint32_t *)(message->data() + index);
			uint32_t length = ntohl(*lengthPtr);
			auto naluStartIndex = index + 4;
			auto naluEndIndex = naluStartIndex + length;

			assert(naluEndIndex <= message->size());
			if (naluEndIndex > message->size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete unit), ignoring!";
				break;
			}
			auto begin = message->begin() + naluStartIndex;
			auto end = message->begin() + naluEndIndex;
			nalus->push_back(std::make_shared<NalUnit>(begin, end));
			index = naluEndIndex;
		}
	} else {
		NalUnitStartSequenceMatch match = NUSM_noMatch;
		unsigned long long index = 0;
		while (index < message->size()) {
			match = StartSequenceMatchSucc(match, (*message)[index++], separator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				match = NUSM_noMatch;
				break;
			}
		}

		unsigned long long naluStartIndex = index;

		while (index < message->size()) {
			match = StartSequenceMatchSucc(match, (*message)[index], separator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				auto sequenceLength = match == NUSM_longMatch ? 4 : 3;
				unsigned long long naluEndIndex = index - sequenceLength;
				match = NUSM_noMatch;
				auto begin = message->begin() + naluStartIndex;
				auto end = message->begin() + naluEndIndex + 1;
				nalus->push_back(std::make_shared<NalUnit>(begin, end));
				naluStartIndex = index + 1;
			}
			index++;
		}
		auto begin = message->begin() + naluStartIndex;
		auto end = message->end();
		nalus->push_back(std::make_shared<NalUnit>(begin, end));
	}
	return nalus;
}

H264RtpPacketizer::H264RtpPacketizer(std::shared_ptr<RtpPacketizationConfig> rtpConfig,
									 uint16_t maximumFragmentSize)
: RtpPacketizer(rtpConfig), MediaHandlerRootElement(), maximumFragmentSize(maximumFragmentSize), separator(Separator::Length) {}

H264RtpPacketizer::H264RtpPacketizer(H264RtpPacketizer::Separator separator, std::shared_ptr<RtpPacketizationConfig> rtpConfig,
									 uint16_t maximumFragmentSize)
: RtpPacketizer(rtpConfig), MediaHandlerRootElement(), maximumFragmentSize(maximumFragmentSize), separator(separator) {}

ChainedOutgoingProduct H264RtpPacketizer::processOutgoingBinaryMessage(ChainedMessagesProduct messages, message_ptr control) {
	ChainedMessagesProduct packets = std::make_shared<std::vector<binary_ptr>>();
	for (auto message: *messages) {
		auto nalus = splitMessage(message);
		auto fragments = nalus->generateFragments(maximumFragmentSize);
		if (fragments.size() == 0) {
			return ChainedOutgoingProduct();
		}
		unsigned i = 0;
		for (; i < fragments.size() - 1; i++) {
			packets->push_back(packetize(fragments[i], false));
		}
		packets->push_back(packetize(fragments[i], true));
	}
	return {packets, control};
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
