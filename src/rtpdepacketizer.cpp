/**
 * Copyright (c) 2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtpdepacketizer.hpp"
#include "rtp.hpp"

#include "impl/logcounter.hpp"

namespace rtc {

RtpDepacketizer::RtpDepacketizer() : mClockRate(0) {}

RtpDepacketizer::RtpDepacketizer(uint32_t clockRate) : mClockRate(clockRate) {}

RtpDepacketizer::~RtpDepacketizer() {}

void RtpDepacketizer::incoming(message_vector &messages,
                               [[maybe_unused]] const message_callback &send) {
	message_vector result;
	for (auto &message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			continue;
		}

		if (message->size() < sizeof(RtpHeader)) {
			PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
			continue;
		}

		auto pkt = reinterpret_cast<const rtc::RtpHeader *>(message->data());
		auto headerSize = sizeof(rtc::RtpHeader) + pkt->csrcCount() + pkt->getExtensionHeaderSize();
		result.push_back(make_message(message->begin() + headerSize, message->end(),
		                              createFrameInfo(pkt->timestamp(), pkt->payloadType())));
	}

	messages.swap(result);
}

shared_ptr<FrameInfo> RtpDepacketizer::createFrameInfo(uint32_t timestamp,
                                                       uint8_t payloadType) const {
	auto frameInfo = std::make_shared<FrameInfo>(timestamp);
	if (mClockRate > 0)
		frameInfo->timestampSeconds =
		    std::chrono::duration<double>(double(timestamp) / double(mClockRate));
	frameInfo->payloadType = payloadType;
	return frameInfo;
}

VideoRtpDepacketizer::VideoRtpDepacketizer() : RtpDepacketizer(ClockRate) {}

VideoRtpDepacketizer::~VideoRtpDepacketizer() {}

void VideoRtpDepacketizer::incoming(message_vector &messages,
                                    [[maybe_unused]] const message_callback &send) {
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
				if (auto frame = reassemble(mBuffer))
					result.push_back(frame);

				mBuffer.clear();
			}
		}

		mBuffer.insert(std::move(message));

		if (header->marker()) {
			if (auto frame = reassemble(mBuffer))
				result.push_back(std::move(frame));

			mBuffer.clear();
		}
	};

	messages.swap(result);
}

bool VideoRtpDepacketizer::sequence_cmp::operator()(message_ptr a, message_ptr b) const {
	assert(a->size() >= sizeof(RtpHeader) && b->size() >= sizeof(RtpHeader));
	auto ha = reinterpret_cast<const rtc::RtpHeader *>(a->data());
	auto hb = reinterpret_cast<const rtc::RtpHeader *>(b->data());
	int16_t d = int16_t(hb->seqNumber() - ha->seqNumber());
	return d > 0;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
