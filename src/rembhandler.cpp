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

namespace rtc {

RembHandler::RembHandler(std::function<void(unsigned int)> onRemb) : mOnRemb(onRemb) {}

void RembHandler::incoming(message_vector &messages, [[maybe_unused]] const message_callback &send) {
	for (const auto &message : messages) {
		size_t offset = 0;
		while ((sizeof(RtcpHeader) + offset) <= message->size()) {
			auto header = reinterpret_cast<RtcpHeader *>(message->data() + offset);
			uint8_t payload_type = header->payloadType();

			if (payload_type == 206 && header->reportCount() == 15 && header->lengthInBytes() == sizeof(RtcpRemb)) {
				auto remb = reinterpret_cast<RtcpRemb *>(message->data() + offset);

				if (remb->_id[0] == 'R' && remb->_id[1] == 'E' && remb->_id[2] == 'M' && remb->_id[3] == 'B') {
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
