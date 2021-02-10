/*
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#if RTC_ENABLE_MEDIA

#include "opusrtppacketizer.hpp"

namespace rtc {

OpusRtpPacketizer::OpusRtpPacketizer(std::shared_ptr<RtpPacketizationConfig> rtpConfig)
: RtpPacketizer(rtpConfig), MediaHandlerRootElement() {}

binary_ptr OpusRtpPacketizer::packetize(binary_ptr payload, [[maybe_unused]] bool setMark) {
	assert(!setMark);
	return RtpPacketizer::packetize(payload, false);
}

ChainedOutgoingProduct OpusRtpPacketizer::processOutgoingBinaryMessage(ChainedMessagesProduct messages, message_ptr control) {
	ChainedMessagesProduct packets = make_chained_messages_product();
	packets->reserve(messages->size());
	for (auto message: *messages) {
		packets->push_back(packetize(message, false));
	}
	return {packets, control};
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
