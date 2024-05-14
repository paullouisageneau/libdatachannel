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

namespace rtc {

const binary naluLongStartCode = {byte{0}, byte{0}, byte{0}, byte{1}};
const binary naluShortStartCode = {byte{0}, byte{0}, byte{1}};

const uint8_t naluTypeSTAPA = 24;
const uint8_t naluTypeFUA = 28;

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

message_vector H264RtpDepacketizer::buildFrames(message_vector::iterator begin,
                                                message_vector::iterator end, uint8_t payloadType,
                                                uint32_t timestamp) {
	message_vector out = {};
	auto accessUnit = binary{};
	auto frameInfo = std::make_shared<FrameInfo>(payloadType, timestamp);

	for (auto it = begin; it != end; ++it) {
		auto pkt = it->get();
		auto pktParsed = reinterpret_cast<const rtc::RtpHeader *>(pkt->data());
		auto rtpHeaderSize = pktParsed->getSize() + pktParsed->getExtensionHeaderSize();
		auto rtpPaddingSize = 0;

		if (pktParsed->padding()) {
			rtpPaddingSize = std::to_integer<uint8_t>(pkt->at(pkt->size() - 1));
		}

		if (pkt->size() == rtpHeaderSize + rtpPaddingSize) {
			PLOG_VERBOSE << "H.264 RTP packet has empty payload";
			continue;
		}

		auto nalUnitHeader = NalUnitHeader{std::to_integer<uint8_t>(pkt->at(rtpHeaderSize))};

		if (nalUnitHeader.unitType() == naluTypeFUA) {
			auto nalUnitFragmentHeader = NalUnitFragmentHeader{
			    std::to_integer<uint8_t>(pkt->at(rtpHeaderSize + sizeof(NalUnitHeader)))};

			// RFC 6184: When set to one, the Start bit indicates the start of a fragmented NAL
			// unit. When the following FU payload is not the start of a fragmented NAL unit
			// payload, the Start bit is set to zero.
			if (nalUnitFragmentHeader.isStart() || accessUnit.empty()) {
				addSeparator(accessUnit);
				accessUnit.emplace_back(
				    byte(nalUnitHeader.idc() | nalUnitFragmentHeader.unitType()));
			}

			accessUnit.insert(accessUnit.end(),
			                  pkt->begin() + rtpHeaderSize + sizeof(NalUnitHeader) +
			                      sizeof(NalUnitFragmentHeader),
			                  pkt->end());
		} else if (nalUnitHeader.unitType() > 0 && nalUnitHeader.unitType() < 24) {
			addSeparator(accessUnit);
			accessUnit.insert(accessUnit.end(), pkt->begin() + rtpHeaderSize, pkt->end());
		} else if (nalUnitHeader.unitType() == naluTypeSTAPA) {
			auto currOffset = rtpHeaderSize + sizeof(NalUnitHeader);

			while (currOffset + sizeof(uint16_t) < pkt->size()) {
				auto naluSize = std::to_integer<uint16_t>(pkt->at(currOffset)) << 8 |
				                std::to_integer<uint16_t>(pkt->at(currOffset + 1));

				currOffset += sizeof(uint16_t);

				if (pkt->size() < currOffset + naluSize) {
					throw std::runtime_error("H264 STAP-A declared size is larger than buffer");
				}

				addSeparator(accessUnit);
				accessUnit.insert(accessUnit.end(), pkt->begin() + currOffset,
				                  pkt->begin() + currOffset + naluSize);

				currOffset += naluSize;
			}
		} else {
			throw std::runtime_error("Unknown H264 RTP Packetization");
		}
	}

	if (!accessUnit.empty()) {
		out.emplace_back(
		    make_message(std::move(accessUnit), Message::Binary, 0, nullptr, frameInfo));
	}

	return out;
}

void H264RtpDepacketizer::incoming(message_vector &messages, const message_callback &) {
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
		uint8_t payload_type = 0;
		uint32_t current_timestamp = 0;
		size_t packets_in_timestamp = 0;

		for (const auto &pkt : mRtpBuffer) {
			auto p = reinterpret_cast<const rtc::RtpHeader *>(pkt->data());

			if (current_timestamp == 0) {
				current_timestamp = p->timestamp();
				payload_type =
				    p->payloadType(); // should all be the same for data of the same codec
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

		auto frames = buildFrames(begin, end + 1, payload_type, current_timestamp);
		messages.insert(messages.end(), frames.begin(), frames.end());
		mRtpBuffer.erase(mRtpBuffer.begin(), mRtpBuffer.begin() + packets_in_timestamp);
	}
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
