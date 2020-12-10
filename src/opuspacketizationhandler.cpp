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

#include "opuspacketizationhandler.hpp"

#if RTC_ENABLE_MEDIA

using namespace rtc;

OpusPacketizationHandler::OpusPacketizationHandler(std::shared_ptr<OpusRTPPacketizer> packetizer): RtcpHandler(), RTCPSenderReportable(packetizer->rtpConfig), packetizer(packetizer) {
    senderReportOutgoingCallback = [this](message_ptr msg) {
        outgoingCallback(msg);
    };
}

message_ptr OpusPacketizationHandler::incoming(message_ptr ptr) {
    return ptr;
}

message_ptr OpusPacketizationHandler::outgoing(message_ptr ptr) {
    if (ptr->type == Message::Binary) {
        return withStatsRecording<message_ptr>([this, ptr](std::function<void (message_ptr)> addToReport) {
            auto rtp = packetizer->packetize(*ptr, false);
            addToReport(rtp);
            return rtp;
        });
    }
    return ptr;
}

#endif /* RTC_ENABLE_MEDIA */
