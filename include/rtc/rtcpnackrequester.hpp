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
	SSRC ssrc;
	size_t jitterSize;
	size_t nackWaitMs;

	RtcpNackRequester(SSRC ssrc, size_t jitterSize = 5, size_t nackWaitMs = 50);
	void incoming(message_vector &messages, const message_callback &send) override;

private:
	uint16_t expectSequence = 0;
	std::chrono::steady_clock::time_point nackWaitUntil;

	std::unordered_map<uint16_t, message_ptr> receivePackets;
	std::set<uint16_t> lostSequenceNumbers;

	message_ptr nackMesssage(uint16_t sequence);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_NACK_RESPONDER_H */
