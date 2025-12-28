/**
 * Copyright (c) 2025 kaizhi-singtown
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTCP_NACK_RESPONDER_H
#define RTC_RTCP_NACK_RESPONDER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "rtp.hpp"

#include <set>
#include <unordered_map>

namespace rtc {

class RTC_CPP_EXPORT RtcpNackRequester final : public MediaHandler {
public:
	RtcpNackRequester(SSRC ssrc, size_t jitterSize = 5, size_t nackResendIntervalMs = 10,
	                  size_t nackResendTimesMax = 10);
	SSRC ssrc;
	void incoming(message_vector &messages, const message_callback &send) override;

private:
	size_t jitterSize;
	size_t nackResendIntervalMs;
	size_t nackResendTimesMax;

	bool initialized = false;
	uint16_t expectedSeq;
	size_t nackResendTimes = 0;
	std::chrono::steady_clock::time_point nextNackTime = std::chrono::steady_clock::now();

	std::map<uint16_t, message_ptr> jitterBuffer;

	auto isSeqNewerOrEqual(uint16_t seq1, uint16_t seq2) -> bool;
	void clearBuffer();
	auto nackMessage(uint16_t sequence) -> message_ptr;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_NACK_RESPONDER_H */
