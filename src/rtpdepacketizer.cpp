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
		result.push_back(make_message(message->begin() + headerSize, message->end(),
		                              Message::Binary, 0, nullptr,
		                              std::make_shared<FrameInfo>(pkt->payloadType(), pkt->timestamp())));
	}

	messages.swap(result);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
