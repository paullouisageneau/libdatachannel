/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpreceivingsession.hpp"
#include "track.hpp"

#include "impl/logcounter.hpp"

#include <cmath>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

static impl::LogCounter COUNTER_BAD_RTP_HEADER(plog::warning, "Number of malformed RTP headers");
static impl::LogCounter COUNTER_UNKNOWN_PPID(plog::warning, "Number of Unknown PPID messages");
static impl::LogCounter COUNTER_BAD_NOTIF_LEN(plog::warning,
                                              "Number of Bad-Lengthed notifications");
static impl::LogCounter COUNTER_BAD_SCTP_STATUS(plog::warning,
                                                "Number of unknown SCTP_STATUS errors");

message_ptr RtcpReceivingSession::outgoing(message_ptr ptr) { return ptr; }

message_ptr RtcpReceivingSession::incoming(message_ptr ptr) {
	if (ptr->type == Message::Binary) {
		auto rtp = reinterpret_cast<const RtpHeader *>(ptr->data());

		// https://www.rfc-editor.org/rfc/rfc3550.html#appendix-A.1
		if (rtp->version() != 2) {
			COUNTER_BAD_RTP_HEADER++;
			PLOG_VERBOSE << "RTP packet is not version 2";

			return nullptr;
		}
		if (rtp->payloadType() == 201 || rtp->payloadType() == 200) {
			COUNTER_BAD_RTP_HEADER++;
			PLOG_VERBOSE << "RTP packet has a payload type indicating RR/SR";

			return nullptr;
		}

		// Padding-processing is a user-level thing

		mSsrc = rtp->ssrc();

		return ptr;
	}

	assert(ptr->type == Message::Control);
	auto rr = reinterpret_cast<const RtcpRr *>(ptr->data());
	if (rr->header.payloadType() == 201) {
		// RR
		mSsrc = rr->senderSSRC();
		rr->log();
	} else if (rr->header.payloadType() == 200) {
		// SR
		mSsrc = rr->senderSSRC();
		auto sr = reinterpret_cast<const RtcpSr *>(ptr->data());
		mSyncRTPTS = sr->rtpTimestamp();
		mSyncNTPTS = sr->ntpTimestamp();
		sr->log();

		// TODO For the time being, we will send RR's/REMB's when we get an SR
		pushRR(0);
		if (mRequestedBitrate > 0)
			pushREMB(mRequestedBitrate);
	}
	return nullptr;
}

void RtcpReceivingSession::requestBitrate(unsigned int newBitrate) {
	mRequestedBitrate = newBitrate;

	PLOG_DEBUG << "[GOOG-REMB] Requesting bitrate: " << newBitrate << std::endl;
	pushREMB(newBitrate);
}

void RtcpReceivingSession::pushREMB(unsigned int bitrate) {
	message_ptr msg = make_message(RtcpRemb::SizeWithSSRCs(1), Message::Control);
	auto remb = reinterpret_cast<RtcpRemb *>(msg->data());
	remb->preparePacket(mSsrc, 1, bitrate);
	remb->setSsrc(0, mSsrc);

	send(msg);
}

void RtcpReceivingSession::pushRR(unsigned int lastSR_delay) {
	auto msg = make_message(RtcpRr::SizeWithReportBlocks(1), Message::Control);
	auto rr = reinterpret_cast<RtcpRr *>(msg->data());
	rr->preparePacket(mSsrc, 1);
	rr->getReportBlock(0)->preparePacket(mSsrc, 0, 0, uint16_t(mGreatestSeqNo), 0, 0, mSyncNTPTS,
	                                     lastSR_delay);
	rr->log();

	send(msg);
}

bool RtcpReceivingSession::send(message_ptr msg) {
	try {
		outgoingCallback(std::move(msg));
		return true;
	} catch (const std::exception &e) {
		LOG_DEBUG << "RTCP tx failed: " << e.what();
	}
	return false;
}

bool RtcpReceivingSession::requestKeyframe() {
	pushPLI();
	return true; // TODO Make this false when it is impossible (i.e. Opus).
}

void RtcpReceivingSession::pushPLI() {
	auto msg = make_message(RtcpPli::Size(), Message::Control);
	auto *pli = reinterpret_cast<RtcpPli *>(msg->data());
	pli->preparePacket(mSsrc);
	send(msg);
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
