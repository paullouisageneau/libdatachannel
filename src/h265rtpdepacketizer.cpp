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

namespace rtc {

const binary naluStartCode = {byte{0}, byte{0}, byte{0}, byte{1}};

const uint8_t naluTypeAP = 48;
const uint8_t naluTypeFU = 49;

message_vector H265RtpDepacketizer::buildFrames(message_vector::iterator begin,
                                                message_vector::iterator end, uint32_t timestamp) {
	message_vector out = {};
	auto accessUnit = binary{};
	auto frameInfo = std::make_shared<FrameInfo>(timestamp);
	auto nFrags = 0;

	for (auto it = begin; it != end; ++it) {
		auto pkt = it->get();
		auto pktParsed = reinterpret_cast<const rtc::RtpHeader *>(pkt->data());
		auto rtpHeaderSize = pktParsed->getSize() + pktParsed->getExtensionHeaderSize();
		auto rtpPaddingSize = 0;

		if (pktParsed->padding()) {
			rtpPaddingSize = std::to_integer<uint8_t>(pkt->at(pkt->size() - 1));
		}

		if (pkt->size() == rtpHeaderSize + rtpPaddingSize) {
			PLOG_VERBOSE << "H.265 RTP packet has empty payload";
			continue;
		}

		auto nalUnitHeader =
		    H265NalUnitHeader{std::to_integer<uint8_t>(pkt->at(rtpHeaderSize)),
		                      std::to_integer<uint8_t>(pkt->at(rtpHeaderSize + 1))};

		if (nalUnitHeader.unitType() == naluTypeFU) {
			auto nalUnitFragmentHeader = H265NalUnitFragmentHeader{
			    std::to_integer<uint8_t>(pkt->at(rtpHeaderSize + sizeof(H265NalUnitHeader)))};

			if (nFrags++ == 0) {
				accessUnit.insert(accessUnit.end(), naluStartCode.begin(), naluStartCode.end());

				nalUnitHeader.setUnitType(nalUnitFragmentHeader.unitType());
				accessUnit.emplace_back(byte(nalUnitHeader._first));
				accessUnit.emplace_back(byte(nalUnitHeader._second));
			}

			accessUnit.insert(accessUnit.end(),
			                  pkt->begin() + rtpHeaderSize + sizeof(H265NalUnitHeader) +
			                      sizeof(H265NalUnitFragmentHeader),
			                  pkt->end());
		} else if (nalUnitHeader.unitType() == naluTypeAP) {
			auto currOffset = rtpHeaderSize + sizeof(H265NalUnitHeader);

			while (currOffset + sizeof(uint16_t) < pkt->size()) {
				auto naluSize = std::to_integer<uint16_t>(pkt->at(currOffset)) << 8 |
				                std::to_integer<uint16_t>(pkt->at(currOffset + 1));

				currOffset += sizeof(uint16_t);

				if (pkt->size() < currOffset + naluSize) {
					throw std::runtime_error("H265 AP declared size is larger than buffer");
				}

				accessUnit.insert(accessUnit.end(), naluStartCode.begin(), naluStartCode.end());

				accessUnit.insert(accessUnit.end(), pkt->begin() + currOffset,
				                  pkt->begin() + currOffset + naluSize);

				currOffset += naluSize;
			}
		} else if (nalUnitHeader.unitType() < naluTypeAP) {
			// "NAL units with NAL unit type values in the range of 0 to 47, inclusive, may be
			// passed to the decoder."
			accessUnit.insert(accessUnit.end(), naluStartCode.begin(), naluStartCode.end());
			accessUnit.insert(accessUnit.end(), pkt->begin() + rtpHeaderSize, pkt->end());
		} else {
			// "NAL-unit-like structures with NAL unit type values in the range of 48 to 63,
			// inclusive, MUST NOT be passed to the decoder."
		}
	}

	if (!accessUnit.empty()) {
		out.emplace_back(make_message(accessUnit.begin(), accessUnit.end(), Message::Binary, 0,
		                              nullptr, frameInfo));
	}

	return out;
}

void H265RtpDepacketizer::incoming(message_vector &messages, const message_callback &) {
	messages.erase(std::remove_if(messages.begin(), messages.end(),
	                              [&](message_ptr message) {
		                              if (message->type == Message::Control) {
			                              return false;
		                              }

		                              if (message->size() < sizeof(RtpHeader)) {
			                              PLOG_VERBOSE << "RTP packet is too small, size="
			                                           << message->size();
			                              return true;
		                              }

		                              mRtpBuffer.push_back(std::move(message));
		                              return true;
	                              }),
	               messages.end());

	while (mRtpBuffer.size() != 0) {
		uint32_t current_timestamp = 0;
		size_t packets_in_timestamp = 0;

		for (const auto &pkt : mRtpBuffer) {
			auto p = reinterpret_cast<const rtc::RtpHeader *>(pkt->data());

			if (current_timestamp == 0) {
				current_timestamp = p->timestamp();
			} else if (current_timestamp != p->timestamp()) {
				break;
			}

			packets_in_timestamp++;
		}

		if (packets_in_timestamp == mRtpBuffer.size()) {
			break;
		}

		auto begin = mRtpBuffer.begin();
		auto end = mRtpBuffer.begin() + (packets_in_timestamp - 1);

		auto frames = buildFrames(begin, end + 1, current_timestamp);
		messages.insert(messages.end(), frames.begin(), frames.end());
		mRtpBuffer.erase(mRtpBuffer.begin(), mRtpBuffer.begin() + packets_in_timestamp);
	}
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
