/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTCP_SR_REPORTER_H
#define RTC_RTCP_SR_REPORTER_H

#if RTC_ENABLE_MEDIA

#include "description.hpp"
#include "mediahandler.hpp"
#include "rtp.hpp"
#include "rtppacketizationconfig.hpp"

#include <atomic>
#include <chrono>

namespace rtc {

class RTC_CPP_EXPORT RtcpSrReporter final : public MediaHandler {
public:
	RtcpSrReporter(shared_ptr<RtpPacketizationConfig> rtpConfig);
	~RtcpSrReporter();

	uint32_t lastReportedTimestamp() const;
	[[deprecated]] void setNeedsToReport();

	void media(const Description::Media &desc) override;
	void outgoing(message_vector &messages, const message_callback &send) override;
	void close(const message_callback &send, Description::Direction direction,
	           bool sentPacket) override;

	// TODO: remove this
	const shared_ptr<RtpPacketizationConfig> rtpConfig;

private:
	void addToReport(RtpHeader *header, size_t size);
	message_ptr getSenderReport(uint32_t timestamp, size_t extraSize = 0);

	std::atomic<SSRC> mRtxSsrc = 0; // RTX SSRC for the media, 0 if none

	uint32_t mPacketCount = 0;
	uint32_t mPayloadOctets = 0;
	uint32_t mLastReportedTimestamp = 0;
	std::chrono::steady_clock::time_point mLastReportTime;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_SR_REPORTER_H */
