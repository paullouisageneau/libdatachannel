/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "apphandler.hpp"

#include "impl/internals.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <cstring>

#if RTC_ENABLE_MEDIA

namespace rtc {

AppHandler::AppHandler(std::function<void(string name, uint8_t subtype, binary data)> onApp)
    : mOnApp(onApp) {}

void AppHandler::incoming(message_vector &messages, [[maybe_unused]] const message_callback &send) {
	for (const auto &message : messages) {
		size_t offset = 0;
		while (offset + sizeof(RtcpHeader) <= message->size()) {
			auto header = reinterpret_cast<RtcpHeader *>(message->data() + offset);
			size_t length = header->lengthInBytes();
			if (offset + length > message->size())
				break;

			if (header->payloadType() == 204) {
				size_t minSize = sizeof(RtcpHeader) + sizeof(SSRC) + 4; // header + SSRC + name
				if (length < minSize)
					break;

				auto app = reinterpret_cast<RtcpApp *>(message->data() + offset);
				uint8_t subtype = app->subtype();
				string name = app->name();
				size_t dataLen = app->dataSize();
				binary data(message->data() + offset + minSize,
				            message->data() + offset + minSize + dataLen);

				PLOG_DEBUG << "Got RTCP APP, name=" << name << " subtype=" << (int)subtype
				           << " dataLen=" << dataLen;
				mOnApp(std::move(name), subtype, std::move(data));
			}

			offset += length;
		}
	}
}

bool AppHandler::sendRtcpApp(SSRC ssrc, const char name[4], uint8_t subtype, const binary &data,
                             const message_callback &send) {
	size_t packetSize = RtcpApp::SizeWithData(data.size());
	auto message = make_message(packetSize, Message::Control);

	auto app = reinterpret_cast<RtcpApp *>(message->data());
	app->preparePacket(ssrc, subtype, name, data.size());

	if (!data.empty())
		std::memcpy(app->_data, data.data(), data.size());

	send(std::move(message));
	return true;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
