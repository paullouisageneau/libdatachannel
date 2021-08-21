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

#include "rtcpsrreporter.hpp"

#include <cassert>
#include <cmath>

namespace rtc {

ChainedOutgoingProduct RtcpSrReporter::processOutgoingBinaryMessage(ChainedMessagesProduct messages,
                                                                    message_ptr control) {
	if (needsToReport) {
		auto timestamp = rtpConfig->timestamp;
		auto sr = getSenderReport(timestamp);
		if (control) {
			control->insert(control->end(), sr->begin(), sr->end());
		} else {
			control = sr;
		}
		needsToReport = false;
	}
	for (auto message : *messages) {
		auto rtp = reinterpret_cast<RTP *>(message->data());
		addToReport(rtp, message->size());
	}
	return {messages, control};
}

void RtcpSrReporter::startRecording() {
	_previousReportedTimestamp = rtpConfig->timestamp;
	timeOffset = rtpConfig->startTime_s - rtpConfig->timestampToSeconds(rtpConfig->timestamp);
}

void RtcpSrReporter::addToReport(RTP *rtp, uint32_t rtpSize) {
	packetCount += 1;
	assert(!rtp->padding());
	payloadOctets += rtpSize - rtp->getSize();
}

RtcpSrReporter::RtcpSrReporter(shared_ptr<RtpPacketizationConfig> rtpConfig)
    : MediaHandlerElement(), rtpConfig(rtpConfig) {}

uint64_t RtcpSrReporter::secondsToNTP(double seconds) {
	return std::round(seconds * double(uint64_t(1) << 32));
}

void RtcpSrReporter::setNeedsToReport() { needsToReport = true; }

message_ptr RtcpSrReporter::getSenderReport(uint32_t timestamp) {
	auto srSize = RTCP_SR::Size(0);
	auto msg = make_message(srSize + RTCP_SDES::Size({{uint8_t(rtpConfig->cname.size())}}),
	                        Message::Type::Control);
	auto sr = reinterpret_cast<RTCP_SR *>(msg->data());
	auto timestamp_s = rtpConfig->timestampToSeconds(timestamp);
	auto currentTime = timeOffset + timestamp_s;
	sr->setNtpTimestamp(secondsToNTP(currentTime));
	sr->setRtpTimestamp(timestamp);
	sr->setPacketCount(packetCount);
	sr->setOctetCount(payloadOctets);
	sr->preparePacket(rtpConfig->ssrc, 0);

	auto sdes = reinterpret_cast<RTCP_SDES *>(msg->data() + srSize);
	auto chunk = sdes->getChunk(0);
	chunk->setSSRC(rtpConfig->ssrc);
	auto item = chunk->getItem(0);
	item->type = 1;
	item->setText(rtpConfig->cname);
	sdes->preparePacket(1);

	_previousReportedTimestamp = timestamp;

	return msg;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
