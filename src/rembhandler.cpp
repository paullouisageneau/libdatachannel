/**
 * Copyright (c) 2024 Vladimir Voronin
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rembhandler.hpp"
#include "rtp.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#if RTC_ENABLE_MEDIA

static inline uint32_t makeEndianAgnosticValue(uint32_t value) {
	return ((uint8_t *)&value)[0] | (((uint8_t *)&value)[1] << 8) | (((uint8_t *)&value)[2] << 16) |
	       (((uint8_t *)&value)[3] << 24);
}

namespace rtc {

RembHandler::RembHandler(std::function<void(unsigned int)> onRemb) : mOnRemb(onRemb) {}

void RembHandler::incoming(message_vector &messages, [[maybe_unused]] const message_callback &send) {
	static uint32_t rembValue = makeEndianAgnosticValue('BMER');

	for (const auto &message : messages) {
		size_t offset = 0;
		while ((sizeof(RtcpHeader) + offset) <= message->size()) {
			auto header = reinterpret_cast<RtcpHeader *>(message->data() + offset);
			uint8_t payload_type = header->payloadType();

			if (payload_type == 206 && header->reportCount() == 15 && header->lengthInBytes() == sizeof(RtcpRemb)) {
				auto remb = reinterpret_cast<RtcpRemb *>(message->data() + offset);

				if (*(reinterpret_cast<const uint32_t *>(&remb->_id)) == rembValue) {
					mOnRemb(remb->getBitrate());
					break;
				}
			}

			offset += header->lengthInBytes();
		}
	}
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
