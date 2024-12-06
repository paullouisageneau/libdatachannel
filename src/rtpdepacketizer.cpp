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

#include <cmath>
#include <cstring>

namespace rtc {

RtpDepacketizer::RtpDepacketizer() : mClockRate(0) {}

RtpDepacketizer::RtpDepacketizer(uint32_t clockRate) : mClockRate(clockRate) {}

RtpDepacketizer::~RtpDepacketizer() {}

void RtpDepacketizer::incoming([[maybe_unused]] message_vector &messages,
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

		auto frameInfo = std::make_shared<FrameInfo>(pkt->timestamp());
		if (mClockRate > 0)
			frameInfo->timestampSeconds =
			    std::chrono::duration<double>(double(pkt->timestamp()) / double(mClockRate));
		frameInfo->payloadType = pkt->payloadType();
		result.push_back(make_message(message->begin() + headerSize, message->end(), frameInfo));
	}

	messages.swap(result);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
