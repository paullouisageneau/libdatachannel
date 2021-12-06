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

#ifndef RTC_RTCP_SENDER_REPORTABLE_H
#define RTC_RTCP_SENDER_REPORTABLE_H

#if RTC_ENABLE_MEDIA

#include "mediahandlerelement.hpp"
#include "message.hpp"
#include "rtppacketizationconfig.hpp"

namespace rtc {

class RTC_CPP_EXPORT RtcpSrReporter final : public MediaHandlerElement {
	bool needsToReport = false;

	uint32_t packetCount = 0;
	uint32_t payloadOctets = 0;
	double timeOffset = 0;

	uint32_t mPreviousReportedTimestamp = 0;

	void addToReport(RtpHeader *rtp, uint32_t rtpSize);
	message_ptr getSenderReport(uint32_t timestamp);

public:
	static uint64_t secondsToNTP(double seconds);

	/// Timestamp of previous sender report
	const uint32_t &previousReportedTimestamp = mPreviousReportedTimestamp;

	/// RTP configuration
	const shared_ptr<RtpPacketizationConfig> rtpConfig;

	RtcpSrReporter(shared_ptr<RtpPacketizationConfig> rtpConfig);

	ChainedOutgoingProduct processOutgoingBinaryMessage(ChainedMessagesProduct messages,
	                                                    message_ptr control) override;

	/// Set `needsToReport` flag. Sender report will be sent before next RTP packet with same
	/// timestamp.
	void setNeedsToReport();

	/// Set offset to compute NTS for RTCP SR packets. Offset represents relation between real start
	/// time and timestamp of the stream in RTP packets
	/// @note `time_offset = rtpConfig->startTime - rtpConfig->timestampToSeconds(rtpConfig->timestamp)`
	void startRecording();
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_SENDER_REPORTABLE_H */
