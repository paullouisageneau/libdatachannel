/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#if RTC_ENABLE_MEDIA

#include "opusrtppacketizer.hpp"

#include <cassert>

namespace rtc {

OpusRtpPacketizer::OpusRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig)
    : RtpPacketizer(rtpConfig), MediaHandlerRootElement() {}

binary_ptr OpusRtpPacketizer::packetize(binary_ptr payload, [[maybe_unused]] bool setMark) {
	assert(!setMark);
	return RtpPacketizer::packetize(payload, false);
}

ChainedOutgoingProduct
OpusRtpPacketizer::processOutgoingBinaryMessage(ChainedMessagesProduct messages,
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
