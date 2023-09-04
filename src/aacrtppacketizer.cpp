/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "aacrtppacketizer.hpp"

#include <cassert>

namespace rtc {

AACRtpPacketizer::AACRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig)
    : RtpPacketizer(rtpConfig), MediaHandlerRootElement() {}

binary_ptr AACRtpPacketizer::packetize(binary_ptr payload, [[maybe_unused]] bool setMark) {
	assert(!setMark);
	return RtpPacketizer::packetize(payload, false);
}

ChainedOutgoingProduct
AACRtpPacketizer::processOutgoingBinaryMessage(ChainedMessagesProduct messages,
                                               message_ptr control) {
	ChainedMessagesProduct packets = make_chained_messages_product();
	packets->reserve(messages->size());
	for (auto message : *messages) {
		packets->push_back(packetize(message, false));
	}
	return {packets, control};
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
