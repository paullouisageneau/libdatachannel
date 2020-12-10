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

#include "h264packetizationhandler.hpp"

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

typedef enum {
    NUSM_noMatch,
    NUSM_firstZero,
    NUSM_secondZero,
    NUSM_thirdZero,
    NUSM_shortMatch,
    NUSM_longMatch
} NalUnitStartSequenceMatch;

NalUnitStartSequenceMatch StartSequenceMatchSucc(NalUnitStartSequenceMatch match, byte _byte, H264PacketizationHandler::Separator separator) {
    assert(separator != H264PacketizationHandler::Separator::Length);
    auto byte = (uint8_t) _byte;
    auto detectShort = separator == H264PacketizationHandler::Separator::ShortStartSequence || separator == H264PacketizationHandler::Separator::StartSequence;
    auto detectLong = separator == H264PacketizationHandler::Separator::LongStartSequence || separator == H264PacketizationHandler::Separator::StartSequence;
    switch (match) {
        case NUSM_noMatch:
            if (byte == 0x00) {
                return NUSM_firstZero;
            }
            break;
        case NUSM_firstZero:
            if (byte == 0x00) {
                return NUSM_secondZero;
            }
            break;
        case NUSM_secondZero:
            if (byte == 0x00 && detectLong) {
                return NUSM_thirdZero;
            } else if (byte == 0x01 && detectShort) {
                return NUSM_shortMatch;
            }
            break;
        case NUSM_thirdZero:
            if (byte == 0x01 && detectLong) {
                return NUSM_longMatch;
            }
            break;
        case NUSM_shortMatch:
            return NUSM_shortMatch;
        case NUSM_longMatch:
            return NUSM_longMatch;
    }
    return NUSM_noMatch;
}

message_ptr H264PacketizationHandler::incoming(message_ptr ptr) {
    return ptr;
}

shared_ptr<NalUnits> H264PacketizationHandler::splitMessage(rtc::message_ptr message) {
    auto nalus = make_shared<NalUnits>();
    if (separator == Separator::Length) {
        unsigned long long index = 0;
        while (index < message->size()) {
            assert(index + 4 < message->size());
            if (index + 4 >= message->size()) {
                LOG_WARNING << "Invalid NAL Unit data (incomplete length), ignoring!";
                break;
            }
            auto lengthPtr = (uint32_t *) (message->data() + index);
            uint32_t length = ntohl(*lengthPtr);
            auto naluStartIndex = index + 4;
            auto naluEndIndex = naluStartIndex + length;

            assert(naluEndIndex <= message->size());
            if (naluEndIndex > message->size()) {
                LOG_WARNING << "Invalid NAL Unit data (incomplete unit), ignoring!";
                break;
            }
            nalus->push_back(NalUnit(message->begin() + naluStartIndex, message->begin() + naluEndIndex));
            index = naluEndIndex;
        }
    } else {
        NalUnitStartSequenceMatch match = NUSM_noMatch;
        unsigned long long index = 0;
        while (index < message->size()) {
            match = StartSequenceMatchSucc(match, (*message)[index++], separator);
            if (match == NUSM_longMatch || match == NUSM_shortMatch) {
                match = NUSM_noMatch;
                break;
            }
        }
        index++;
        unsigned long long naluStartIndex = index;

        while (index < message->size()) {
            match = StartSequenceMatchSucc(match, (*message)[index], separator);
            if (match == NUSM_longMatch || match == NUSM_shortMatch) {
                auto sequenceLength = match == NUSM_longMatch ? 4 : 3;
                unsigned long long naluEndIndex = index - sequenceLength;
                match = NUSM_noMatch;
                nalus->push_back(NalUnit(message->begin() + naluStartIndex, message->begin() + naluEndIndex + 1));
                naluStartIndex = index + 1;
            }
            index++;
        }
        nalus->push_back(NalUnit(message->begin() + naluStartIndex, message->end()));
    }
    return nalus;
}

message_ptr H264PacketizationHandler::outgoing(message_ptr ptr) {
    if (ptr->type == Message::Binary) {
        auto nalus = splitMessage(ptr);
        auto fragments = nalus->generateFragments(maximumFragmentSize);

         auto lastPacket = withStatsRecording<message_ptr>([fragments, this](function<void (message_ptr)> addToReport) {
             for(unsigned long long index = 0; index < fragments.size() - 1; index++) {
                 auto packet = packetizer->packetize(fragments[index], false);

                 addToReport(packet);

                 outgoingCallback(std::move(packet));
             }
             // packet is last, marker must be set
             auto lastPacket = packetizer->packetize(fragments[fragments.size() - 1], true);
             addToReport(lastPacket);
             return lastPacket;
         });
        return lastPacket;
    }
    return ptr;
}

H264PacketizationHandler::H264PacketizationHandler(Separator separator,
                                                   std::shared_ptr<H264RTPPacketizer> packetizer,
                                                   uint16_t maximumFragmentSize):
        RtcpHandler(),
        rtc::RTCPSenderReportable(packetizer->rtpConfig),
        packetizer(packetizer),
        maximumFragmentSize(maximumFragmentSize),
        separator(separator) {

    senderReportOutgoingCallback = [this](message_ptr msg) {
        outgoingCallback(msg);
    };
}

#endif /* RTC_ENABLE_MEDIA */
