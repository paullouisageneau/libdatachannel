/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#include "rtcp.hpp"

#include <cmath>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif


namespace rtc {


void RtcpSession::onOutgoing(std::function<void(rtc::message_ptr)> cb) { mTxCallback = cb; }

std::optional<rtc::message_ptr> RtcpSession::incoming(rtc::message_ptr ptr) {
	if (ptr->type == rtc::Message::Type::Binary) {
		auto rtp = reinterpret_cast<const RTP *>(ptr->data());

		// https://tools.ietf.org/html/rfc3550#appendix-A.1
		if (rtp->version() != 2) {
			PLOG_WARNING << "RTP packet is not version 2";

			return std::nullopt;
		}
		if (rtp->payloadType() == 201 || rtp->payloadType() == 200) {
			PLOG_WARNING << "RTP packet has a payload type indicating RR/SR";

			return std::nullopt;
		}

		// TODO Implement the padding bit
		if (rtp->padding()) {
			PLOG_WARNING << "Padding processing not implemented";
		}

		mSsrc = ntohl(rtp->ssrc);

		uint32_t seqNo = rtp->seqNumber();
		// uint32_t rtpTS = rtp->getTS();

		if (mGreatestSeqNo < seqNo)
			mGreatestSeqNo = seqNo;

		return ptr;
	}

	assert(ptr->type == rtc::Message::Type::Control);
	auto rr = reinterpret_cast<const RTCP_RR *>(ptr->data());
	if (rr->header.payloadType() == 201) {
		// RR
		mSsrc = rr->getSenderSSRC();
		rr->log();
	} else if (rr->header.payloadType() == 200) {
		// SR
		mSsrc = rr->getSenderSSRC();
		auto sr = reinterpret_cast<const RTCP_SR *>(ptr->data());
		mSyncRTPTS = sr->rtpTimestamp();
		mSyncNTPTS = sr->ntpTimestamp();
		sr->log();

		// TODO For the time being, we will send RR's/REMB's when we get an SR
		pushRR(0);
		if (mRequestedBitrate > 0)
			pushREMB(mRequestedBitrate);
	}
	return std::nullopt;
}

void RtcpSession::requestBitrate(unsigned int newBitrate) {
	mRequestedBitrate = newBitrate;

	PLOG_DEBUG << "[GOOG-REMB] Requesting bitrate: " << newBitrate << std::endl;
	pushREMB(newBitrate);
}

void RtcpSession::pushREMB(unsigned int bitrate) {
	rtc::message_ptr msg =
	    rtc::make_message(RTCP_REMB::sizeWithSSRCs(1), rtc::Message::Type::Control);
	auto remb = reinterpret_cast<RTCP_REMB *>(msg->data());
	remb->preparePacket(mSsrc, 1, bitrate);
	remb->setSSRC(0, mSsrc);
	remb->log();

	tx(msg);
}

void RtcpSession::pushRR(unsigned int lastSR_delay) {
	auto msg = rtc::make_message(RTCP_RR::sizeWithReportBlocks(1), rtc::Message::Type::Control);
	auto rr = reinterpret_cast<RTCP_RR *>(msg->data());
	rr->preparePacket(mSsrc, 1);
	rr->getReportBlock(0)->preparePacket(mSsrc, 0, 0, mGreatestSeqNo, 0, 0, mSyncNTPTS,
	                                     lastSR_delay);
	rr->log();

	tx(msg);
}

void RtcpSession::tx(message_ptr msg) {
	try {
		mTxCallback(msg);
	} catch (const std::exception &e) {
		LOG_DEBUG << "RTCP tx failed: " << e.what();
	}
}

} // namespace rtc

