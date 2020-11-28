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

#include "track.hpp"
#include <cmath>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

rtc::message_ptr RtcpReceivingSession::outgoing(rtc::message_ptr ptr) { return ptr; }

rtc::message_ptr RtcpReceivingSession::incoming(rtc::message_ptr ptr) {
	if (ptr->type == rtc::Message::Type::Binary) {
		auto rtp = reinterpret_cast<const RTP *>(ptr->data());

		// https://tools.ietf.org/html/rfc3550#appendix-A.1
		if (rtp->version() != 2) {
			PLOG_WARNING << "RTP packet is not version 2";

			return nullptr;
		}
		if (rtp->payloadType() == 201 || rtp->payloadType() == 200) {
			PLOG_WARNING << "RTP packet has a payload type indicating RR/SR";

			return nullptr;
		}

		// TODO Implement the padding bit
		if (rtp->padding()) {
			PLOG_WARNING << "Padding processing not implemented";
		}

		mSsrc = rtp->ssrc();

		return ptr;
	}

	assert(ptr->type == rtc::Message::Type::Control);
	auto rr = reinterpret_cast<const RTCP_RR *>(ptr->data());
	if (rr->header.payloadType() == 201) {
		// RR
		mSsrc = rr->senderSSRC();
		rr->log();
	} else if (rr->header.payloadType() == 200) {
		// SR
		mSsrc = rr->senderSSRC();
		auto sr = reinterpret_cast<const RTCP_SR *>(ptr->data());
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
	rtc::message_ptr msg =
	    rtc::make_message(RTCP_REMB::sizeWithSSRCs(1), rtc::Message::Type::Control);
	auto remb = reinterpret_cast<RTCP_REMB *>(msg->data());
	remb->preparePacket(mSsrc, 1, bitrate);
	remb->setSsrc(0, mSsrc);

	send(msg);
}

void RtcpReceivingSession::pushRR(unsigned int lastSR_delay) {
	auto msg = rtc::make_message(RTCP_RR::sizeWithReportBlocks(1), rtc::Message::Type::Control);
	auto rr = reinterpret_cast<RTCP_RR *>(msg->data());
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
	auto msg = rtc::make_message(rtc::RTCP_PLI::size(), rtc::Message::Type::Control);
	auto *pli = reinterpret_cast<rtc::RTCP_PLI *>(msg->data());
	pli->preparePacket(mSsrc);
	send(msg);
}

void RtcpHandler::onOutgoing(const std::function<void(rtc::message_ptr)> &cb) {
	this->outgoingCallback = synchronized_callback<rtc::message_ptr>(cb);
}
} // namespace rtc
