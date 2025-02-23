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

H265RtpPacketizer::H265RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
                                     size_t maxFragmentSize)
    : RtpPacketizer(std::move(rtpConfig)), mSeparator(NalUnit::Separator::Length),
      mMaxFragmentSize(maxFragmentSize) {}

H265RtpPacketizer::H265RtpPacketizer(NalUnit::Separator separator,
                                     shared_ptr<RtpPacketizationConfig> rtpConfig,
                                     size_t maxFragmentSize)
    : RtpPacketizer(std::move(rtpConfig)), mSeparator(separator), mMaxFragmentSize(maxFragmentSize) {}

std::vector<binary> H265RtpPacketizer::fragment(binary data) {
	return H265NalUnit::GenerateFragments(splitFrame(data), mMaxFragmentSize);
}

std::vector<H265NalUnit> H265RtpPacketizer::splitFrame(const binary &frame) {
	std::vector<H265NalUnit> nalus;
	if (mSeparator == NalUnit::Separator::Length) {
		size_t index = 0;
		while (index < frame.size()) {
			assert(index + 4 < frame.size());
			if (index + 4 >= frame.size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete length), ignoring!";
				break;
			}
			uint32_t length;
			std::memcpy(&length, frame.data() + index, sizeof(uint32_t));
			length = ntohl(length);
			auto naluStartIndex = index + 4;
			auto naluEndIndex = naluStartIndex + length;

			assert(naluEndIndex <= frame.size());
			if (naluEndIndex > frame.size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete unit), ignoring!";
				break;
			}
			auto begin = frame.begin() + naluStartIndex;
			auto end = frame.begin() + naluEndIndex;
			nalus.emplace_back(begin, end);
			index = naluEndIndex;
		}
	} else {
		NalUnitStartSequenceMatch match = NUSM_noMatch;
		size_t index = 0;
		while (index < frame.size()) {
			match = NalUnit::StartSequenceMatchSucc(match, frame[index++], mSeparator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				match = NUSM_noMatch;
				break;
			}
		}

		size_t naluStartIndex = index;

		while (index < frame.size()) {
			match = NalUnit::StartSequenceMatchSucc(match, frame[index], mSeparator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				auto sequenceLength = match == NUSM_longMatch ? 4 : 3;
				size_t naluEndIndex = index - sequenceLength;
				match = NUSM_noMatch;
				auto begin = frame.begin() + naluStartIndex;
				auto end = frame.begin() + naluEndIndex + 1;
				nalus.emplace_back(begin, end);
				naluStartIndex = index + 1;
			}
			index++;
		}
		auto begin = frame.begin() + naluStartIndex;
		auto end = frame.end();
		nalus.emplace_back(begin, end);
	}
	return nalus;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
