/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpsrreporter.hpp"

#include <cassert>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

namespace {

// TODO: move to utils
uint64_t ntp_time() {
	const auto now = std::chrono::system_clock::now();
	const double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
	// Assume the epoch is 01/01/1970 and adds the number of seconds between 1900 and 1970
	return uint64_t(std::floor((secs + 2208988800.) * double(uint64_t(1) << 32)));
}

} // namespace

namespace rtc {

RtcpSrReporter::RtcpSrReporter(shared_ptr<RtpPacketizationConfig> rtpConfig) : rtpConfig(rtpConfig) {}

RtcpSrReporter::~RtcpSrReporter() {}

void RtcpSrReporter::setNeedsToReport() {
	// Dummy
}

uint32_t RtcpSrReporter::lastReportedTimestamp() const { return mLastReportedTimestamp; }

void RtcpSrReporter::outgoing(message_vector &messages, const message_callback &send) {
	if (messages.empty())
		return;

	uint32_t timestamp = 0;
	for (const auto &message : messages) {
		if (message->type == Message::Control)
			continue;

		if (message->size() < sizeof(RtpHeader))
			continue;

		auto header = reinterpret_cast<RtpHeader *>(message->data());
		if(header->ssrc() != rtpConfig->ssrc)
			continue;

		timestamp = header->timestamp();

		addToReport(header, message->size());
	}

	auto now = std::chrono::steady_clock::now();
	if (now >= mLastReportTime + 1s) {
		send(getSenderReport(timestamp));
		mLastReportedTimestamp = timestamp;
		mLastReportTime = now;
	}
}

void RtcpSrReporter::addToReport(RtpHeader *header, size_t size) {
	mPacketCount += 1;
	assert(!header->padding());
	mPayloadOctets += uint32_t(size - header->getSize());
}

message_ptr RtcpSrReporter::getSenderReport(uint32_t timestamp) {
	auto srSize = RtcpSr::Size(0);
	auto msg = make_message(srSize + RtcpSdes::Size({{uint8_t(rtpConfig->cname.size())}}),
	                        Message::Control);
	auto sr = reinterpret_cast<RtcpSr *>(msg->data());
	sr->setNtpTimestamp(ntp_time());
	sr->setRtpTimestamp(timestamp);
	sr->setPacketCount(mPacketCount);
	sr->setOctetCount(mPayloadOctets);
	sr->preparePacket(rtpConfig->ssrc, 0);

	auto sdes = reinterpret_cast<RtcpSdes *>(msg->data() + srSize);
	auto chunk = sdes->getChunk(0);
	chunk->setSSRC(rtpConfig->ssrc);
	auto item = chunk->getItem(0);
	item->type = 1;
	item->setText(rtpConfig->cname);
	sdes->preparePacket(1);

	return msg;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
