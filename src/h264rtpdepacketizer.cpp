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
#include "track.hpp"

#include "impl/logcounter.hpp"

#include <cmath>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

const unsigned long stapaHeaderSize = 1;
const auto fuaHeaderSize = 2;

const uint8_t naluTypeSTAPA = 24;
const uint8_t naluTypeFUA = 28;

message_vector H264RtpDepacketizer::buildFrames(message_vector::iterator begin,
                                                message_vector::iterator end, uint8_t payloadType, uint32_t timestamp) {
	message_vector out = {};
	auto fua_buffer = std::vector<std::byte>{};
	auto frameInfo = std::make_shared<FrameInfo>(payloadType, timestamp);

	for (auto it = begin; it != end; it++) {
		auto pkt = it->get();
		auto pktParsed = reinterpret_cast<const rtc::RtpHeader *>(pkt->data());
		auto headerSize =
		    sizeof(rtc::RtpHeader) + pktParsed->csrcCount() + pktParsed->getExtensionHeaderSize();
		auto paddingSize = 0;

		if (pktParsed->padding()) {
			paddingSize = std::to_integer<uint8_t>(pkt->at(pkt->size() - 1));
		}

		if (pkt->size() == headerSize + paddingSize) {
			PLOG_VERBOSE << "H.264 RTP packet has empty payload";
			continue;
		}

		auto nalUnitHeader = NalUnitHeader{std::to_integer<uint8_t>(pkt->at(headerSize))};

		if (fua_buffer.size() != 0 || nalUnitHeader.unitType() == naluTypeFUA) {
			if (fua_buffer.size() == 0) {
				fua_buffer.push_back(std::byte(0));
			}

			auto nalUnitFragmentHeader =
			    NalUnitFragmentHeader{std::to_integer<uint8_t>(pkt->at(headerSize + 1))};

			std::copy(pkt->begin() + headerSize + fuaHeaderSize, pkt->end(),
			          std::back_inserter(fua_buffer));

			if (nalUnitFragmentHeader.isEnd()) {
				fua_buffer.at(0) =
				    std::byte(nalUnitHeader.idc() | nalUnitFragmentHeader.unitType());

				out.push_back(
				    make_message(std::move(fua_buffer), Message::Binary, 0, nullptr, frameInfo));
				fua_buffer.clear();
			}
		} else if (nalUnitHeader.unitType() > 0 && nalUnitHeader.unitType() < 24) {
			out.push_back(make_message(pkt->begin() + headerSize, pkt->end(), Message::Binary, 0,
			                           nullptr, frameInfo));
		} else if (nalUnitHeader.unitType() == naluTypeSTAPA) {
			auto currOffset = stapaHeaderSize + headerSize;

			while (currOffset < pkt->size()) {
				auto naluSize =
				    uint16_t(pkt->at(currOffset)) << 8 | uint8_t(pkt->at(currOffset + 1));

				currOffset += 2;

				if (pkt->size() < currOffset + naluSize) {
					throw std::runtime_error("STAP-A declared size is larger then buffer");
				}

				out.push_back(make_message(pkt->begin() + currOffset,
				                           pkt->begin() + currOffset + naluSize, Message::Binary, 0,
				                           nullptr, frameInfo));
				currOffset += naluSize;
			}
		} else {
			throw std::runtime_error("Unknown H264 RTP Packetization");
		}
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
				payload_type = p->payloadType(); // should all be the same for data of the same codec
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
