/**
 * Copyright (c) 2020 kaizhi-singtown
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpnackrequester.hpp"
#include "rtp.hpp"

#include "impl/internals.hpp"

namespace rtc {

RtcpNackRequester::RtcpNackRequester(SSRC ssrc, size_t jitterSize, size_t nackWaitMs)
    : ssrc(ssrc), jitterSize(jitterSize), nackWaitMs(nackWaitMs) {}

void RtcpNackRequester::incoming(message_vector &messages, const message_callback &send) {
	message_vector result;
	for (const auto &message : messages) {
		if (message->type != Message::Binary) {
			result.push_back(message);
			continue;
		}

		if (message->size() < sizeof(RtpHeader)) {
			result.push_back(message);
			continue;
		}

		auto rtp = reinterpret_cast<RtpHeader *>(message->data());
		uint16_t seqNo = rtp->seqNumber();
		lostSequenceNumbers.erase(seqNo);

		if (expectSequence == 0) {
			expectSequence = seqNo;
		}
		if ((int16_t)(seqNo - expectSequence) >= 0) {
			receivePackets[seqNo] = message;
		}
	}

	while (receivePackets.size() > jitterSize) {
		bool alreadyReceived = receivePackets.count(expectSequence) > 0;
		if (alreadyReceived) {
			auto packet = receivePackets[expectSequence];
			result.push_back(packet);
			receivePackets.erase(expectSequence);
			expectSequence++;
			continue;
		} else {
			bool alreadySentNack = lostSequenceNumbers.count(expectSequence) > 0;
			auto now = std::chrono::steady_clock::now();
			if (alreadySentNack) {
				if (now >= nackWaitUntil) {
					PLOG_VERBOSE << "Skip NACK for lost packet: " << expectSequence;
					expectSequence++;
				}
			} else {
				PLOG_VERBOSE << "Sending NACK for lost packet: " << expectSequence;
				lostSequenceNumbers.insert(expectSequence);
				nackWaitUntil = now + std::chrono::milliseconds(nackWaitMs);
				send(nackMesssage(expectSequence));
			}
			break;
		}
	}
	messages.swap(result);
}

message_ptr RtcpNackRequester::nackMesssage(uint16_t sequence) {
	unsigned int fciCount = 0;
	uint16_t fciPID = 0;

	message_ptr message = make_message(RtcpNack::Size(1), Message::Control);
	auto *nack = reinterpret_cast<RtcpNack *>(message->data());
	nack->preparePacket(ssrc, 1);
	nack->addMissingPacket(&fciCount, &fciPID, sequence);

	return message;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
