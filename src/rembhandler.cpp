/**
 * Copyright (c) 2024 Vladimir Voronin
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rembhandler.hpp"
#include "rtp.hpp"

#include "impl/internals.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#if RTC_ENABLE_MEDIA

namespace rtc {

RembHandler::RembHandler(std::function<void(unsigned int)> onRemb) : mOnRemb(onRemb) {}

void RembHandler::incoming(message_vector &messages, [[maybe_unused]] const message_callback &send) {
	for (const auto &message : messages) {
		size_t offset = 0;
		while (offset + sizeof(RtcpHeader) <= message->size()) {
			auto header = reinterpret_cast<RtcpHeader *>(message->data() + offset);
			size_t length = header->lengthInBytes();
			if (offset + length > message->size())
				break;

			if (header->payloadType() == 206 && header->reportCount() == 15 && length >= sizeof(RtcpRemb)) {
				if (offset + sizeof(RtcpRemb) > message->size())
					break;

				auto remb = reinterpret_cast<RtcpRemb *>(message->data() + offset);
				if (remb->hasValidId()) {
					unsigned int bitrate = remb->getBitrate();
					PLOG_DEBUG << "Got REMB, bitrate=" << bitrate;
					mOnRemb(bitrate);
					break;
				}
			}

			offset += header->lengthInBytes();
		}
	}
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
