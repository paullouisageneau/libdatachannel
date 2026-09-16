/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpsrreporter.hpp"
#include "impl/utils.hpp"

#include <cassert>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

namespace rtc {

RtcpSrReporter::RtcpSrReporter(shared_ptr<RtpPacketizationConfig> rtpConfig) : rtpConfig(rtpConfig) {}

RtcpSrReporter::~RtcpSrReporter() {}

void RtcpSrReporter::setNeedsToReport() {
	// Dummy
}

uint32_t RtcpSrReporter::lastReportedTimestamp() const { return mLastReportedTimestamp; }

void RtcpSrReporter::media(const Description::Media &desc) {
	// An RFC 4588 RTX stream is a second SSRC of the same flow, so this handler owns it too
	mRtxSsrc.store(desc.getRtxSsrcForSsrc(rtpConfig->ssrc).value_or(0));
}

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

void RtcpSrReporter::close(const message_callback &send, Description::Direction direction,
                           bool sentPacket) {
	// RFC 3550 section 6.3.7: a participant which never sent an RTP or RTCP packet must not send
	// a BYE when it leaves
	if (!sentPacket)
		return;

	// The SSRC here is the local sending SSRC, so this handler sends the BYE on any media with
	// a send direction, and a handler holding only a receive-side SSRC defers to it. RFC 8866
	// section 6.7: a media with no direction attribute is sendrecv, so only Inactive and RecvOnly
	// are excluded. A zero SSRC means none was ever configured.
	if (direction == Description::Direction::RecvOnly ||
	    direction == Description::Direction::Inactive || rtpConfig->ssrc == 0)
		return;

	// RFC 3550 section 6.1: a compound RTCP packet must begin with a report packet, "even if the
	// only other RTCP packet in the compound packet is a BYE". The BYE is therefore appended to a
	// real sender report rather than sent on its own.
	//
	// The RTX SSRC is listed alongside the primary one: section 6.3.7 constrains whether a
	// participant may send a BYE at all, not which sources it may list, and the peer knows the RTX
	// SSRC from the negotiated media whether or not it ever carried a packet. The primary is listed
	// first, as some implementations stop parsing the source list at the first SSRC they do not
	// know.
	SSRC rtxSsrc = mRtxSsrc.load();
	uint8_t ssrcCount = rtxSsrc != 0 ? 2 : 1;

	auto byeSize = RtcpBye::SizeWithSSRCs(ssrcCount);
	auto msg = getSenderReport(mLastReportedTimestamp, byeSize);
	auto bye = reinterpret_cast<RtcpBye *>(msg->data() + msg->size() - byeSize);
	bye->preparePacket(ssrcCount);
	bye->setSSRC(0, rtpConfig->ssrc);
	if (rtxSsrc != 0)
		bye->setSSRC(1, rtxSsrc);
	bye->log();
	send(std::move(msg));
}

void RtcpSrReporter::addToReport(RtpHeader *header, size_t size) {
	mPacketCount += 1;
	assert(!header->padding());
	mPayloadOctets += uint32_t(size - header->getSize());
}

message_ptr RtcpSrReporter::getSenderReport(uint32_t timestamp, size_t extraSize) {
	auto srSize = RtcpSr::Size(0);
	auto msg = make_message(srSize + RtcpSdes::Size({{uint8_t(rtpConfig->cname.size())}}) + extraSize,
	                        Message::Control);
	auto sr = reinterpret_cast<RtcpSr *>(msg->data());
	sr->setNtpTimestamp(impl::utils::ntp_time());
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
