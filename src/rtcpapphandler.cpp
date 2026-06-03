/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtcpapphandler.hpp"

#include "impl/internals.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <cstring>

#if RTC_ENABLE_MEDIA

namespace rtc {

RtcpAppHandler::RtcpAppHandler(
    std::function<void(const RtcpAppName name, uint8_t subtype, binary data)> onApp)
    : mOnApp(onApp) {}

void RtcpAppHandler::incoming(message_vector &messages,
                              [[maybe_unused]] const message_callback &send) {
	for (const auto &message : messages) {
		size_t offset = 0;
		while (offset + sizeof(RtcpHeader) <= message->size()) {
			auto header = reinterpret_cast<RtcpHeader *>(message->data() + offset);
			size_t length = header->lengthInBytes();
			if (offset + length > message->size())
				break;

			if (header->payloadType() == 204) {
				size_t minSize = RtcpApp::SizeWithData(0); // header + SSRC + name
				if (length < minSize)
					break;

				auto app = reinterpret_cast<RtcpApp *>(message->data() + offset);
				uint8_t subtype = app->subtype();
				RtcpAppName name = app->name();
				size_t dataLen = app->dataSize();
				auto dataBytes = reinterpret_cast<const byte *>(app->data());
				binary data(dataBytes, dataBytes + dataLen);

				PLOG_DEBUG << "Got RTCP APP, name=" << name << " subtype=" << (int)subtype
				           << " dataLen=" << dataLen;
				mOnApp(std::move(name), subtype, std::move(data));
			}

			offset += length;
		}
	}
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
