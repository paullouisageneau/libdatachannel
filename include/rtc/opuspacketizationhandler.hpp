/*
 * libdatachannel client example
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

#ifndef RTC_OPUS_PACKETIZATION_HANDLER_H
#define RTC_OPUS_PACKETIZATION_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "opusrtppacketizer.hpp"
#include "mediachainablehandler.hpp"

namespace rtc {

/// Handler for opus packetization
class RTC_CPP_EXPORT OpusPacketizationHandler : public MediaChainableHandler {

public:
	/// Construct handler for opus packetization.
	/// @param packetizer RTP packetizer for opus
	OpusPacketizationHandler(std::shared_ptr<OpusRtpPacketizer> packetizer);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_OPUS_PACKETIZATION_HANDLER_H */
