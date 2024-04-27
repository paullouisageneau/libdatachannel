/**
 * Copyright (c) 2023 Zita Liao (Dolby)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "h265rtppacketizer.hpp"

#include "impl/internals.hpp"

#include <cassert>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

shared_ptr<H265NalUnits> H265RtpPacketizer::splitMessage(binary_ptr message) {
	auto nalus = std::make_shared<H265NalUnits>();
	if (separator == NalUnit::Separator::Length) {
		size_t index = 0;
		while (index < message->size()) {
			assert(index + 4 < message->size());
			if (index + 4 >= message->size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete length), ignoring!";
				break;
			}
			uint32_t length;
			std::memcpy(&length, message->data() + index, sizeof(uint32_t));
			length = ntohl(length);
			auto naluStartIndex = index + 4;
			auto naluEndIndex = naluStartIndex + length;

			assert(naluEndIndex <= message->size());
			if (naluEndIndex > message->size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete unit), ignoring!";
				break;
			}
			auto begin = message->begin() + naluStartIndex;
			auto end = message->begin() + naluEndIndex;
			nalus->push_back(std::make_shared<H265NalUnit>(begin, end));
			index = naluEndIndex;
		}
	} else {
		NalUnitStartSequenceMatch match = NUSM_noMatch;
		size_t index = 0;
		while (index < message->size()) {
			match = NalUnit::StartSequenceMatchSucc(match, (*message)[index++], separator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				match = NUSM_noMatch;
				break;
			}
		}

		size_t naluStartIndex = index;

		while (index < message->size()) {
			match = NalUnit::StartSequenceMatchSucc(match, (*message)[index], separator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				auto sequenceLength = match == NUSM_longMatch ? 4 : 3;
				size_t naluEndIndex = index - sequenceLength;
				match = NUSM_noMatch;
				auto begin = message->begin() + naluStartIndex;
				auto end = message->begin() + naluEndIndex + 1;
				nalus->push_back(std::make_shared<H265NalUnit>(begin, end));
				naluStartIndex = index + 1;
			}
			index++;
		}
		auto begin = message->begin() + naluStartIndex;
		auto end = message->end();
		nalus->push_back(std::make_shared<H265NalUnit>(begin, end));
	}
	return nalus;
}

H265RtpPacketizer::H265RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
                                     uint16_t maxFragmentSize)
    : RtpPacketizer(std::move(rtpConfig)), maxFragmentSize(maxFragmentSize),
      separator(NalUnit::Separator::Length) {}

H265RtpPacketizer::H265RtpPacketizer(NalUnit::Separator separator,
                                     shared_ptr<RtpPacketizationConfig> rtpConfig,
                                     uint16_t maxFragmentSize)
    : RtpPacketizer(std::move(rtpConfig)), maxFragmentSize(maxFragmentSize),
      separator(separator) {}

void H265RtpPacketizer::outgoing(message_vector &messages, [[maybe_unused]] const message_callback &send) {
	message_vector result;
	for (const auto &message : messages) {
		auto nalus = splitMessage(message);
		auto fragments = nalus->generateFragments(maxFragmentSize);
		if (fragments.size() == 0)
			continue;

		for (size_t i = 0; i < fragments.size() - 1; i++)
			result.push_back(packetize(fragments[i], false));

		result.push_back(packetize(fragments[fragments.size() - 1], true));
	}

	messages.swap(result);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
