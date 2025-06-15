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

void RtcpReceivingSession::incoming(message_vector &messages, const message_callback &send) {
	message_vector result;
	for (auto message : messages) {
		switch (message->type) {
		case Message::Binary: {
			if (message->size() < sizeof(RtpHeader)) {
				COUNTER_BAD_RTP_HEADER++;
				PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
				continue;
			}

			auto rtp = reinterpret_cast<const RtpHeader *>(message->data());

			// https://www.rfc-editor.org/rfc/rfc3550.html#appendix-A.1
			if (rtp->version() != 2) {
				COUNTER_BAD_RTP_HEADER++;
				PLOG_VERBOSE << "RTP packet is not version 2";
				continue;
			}

			if (rtp->payloadType() == 201 || rtp->payloadType() == 200) {
				COUNTER_BAD_RTP_HEADER++;
				PLOG_VERBOSE << "RTP packet has a payload type indicating RR/SR";
				continue;
			}

			mSsrc = rtp->ssrc();

			updateSeq(rtp->seqNumber());

			result.push_back(std::move(message));
			break;
		}

		case Message::Control: {
			auto rr = reinterpret_cast<const RtcpRr *>(message->data());
			if (rr->header.payloadType() == 201) { // RR
				mSsrc = rr->senderSSRC();
				rr->log();
			} else if (rr->header.payloadType() == 200) { // SR
				mSsrc = rr->senderSSRC();
				auto sr = reinterpret_cast<const RtcpSr *>(message->data());
				mSyncRTPTS = sr->rtpTimestamp();
				mSyncNTPTS = sr->ntpTimestamp();
				sr->log();

				// TODO For the time being, we will send RR's/REMB's when we get an SR
				pushRR(send, 0);
				if (unsigned int bitrate = mRequestedBitrate.load(); bitrate > 0)
					pushREMB(send, bitrate);
			}
			break;
		}

		default:
			break;
		}
	}

	messages.swap(result);
}

bool RtcpReceivingSession::requestBitrate(unsigned int bitrate, const message_callback &send) {
	PLOG_DEBUG << "Requesting bitrate: " << bitrate << std::endl;
	mRequestedBitrate.store(bitrate);
	pushREMB(send, bitrate);
	return true;
}

void RtcpReceivingSession::pushREMB(const message_callback &send, unsigned int bitrate) {
	auto message = make_message(RtcpRemb::SizeWithSSRCs(1), Message::Control);
	auto remb = reinterpret_cast<RtcpRemb *>(message->data());
	remb->preparePacket(mSsrc, 1, bitrate);
	remb->setSsrc(0, mSsrc);
	send(message);
}

void RtcpReceivingSession::pushRR(const message_callback &send, unsigned int lastSrDelay) {
	auto message = make_message(RtcpRr::SizeWithReportBlocks(1), Message::Control);
	auto rr = reinterpret_cast<RtcpRr *>(message->data());
	rr->preparePacket(mSsrc, 1);

	// calculate packets lost, packet expected, fraction
	auto extended_max = mCycles + mMaxSeq;
    auto expected = extended_max - mBaseSeq + 1;
	auto lost = 0;
	if (mReceived > 0) {
		lost = expected - mReceived;
	} 

	auto expected_interval = expected - mExpectedPrior;
    mExpectedPrior = expected;
    auto received_interval = mReceived - mReceivedPrior;
    mReceivedPrior = mReceived;
    auto lost_interval = expected_interval - received_interval;

	uint8_t fraction;

	if (expected_interval == 0 || lost_interval <= 0) {  
		fraction = 0;
	}	
	else { 
		fraction = (lost_interval << 8) / expected_interval;
	}

	rr->getReportBlock(0)->preparePacket(mSsrc, fraction, lost, uint16_t(mGreatestSeqNo), mMaxSeq, 0, mSyncNTPTS,
	                                     lastSrDelay);
	rr->log();
	send(message);
}

bool RtcpReceivingSession::requestKeyframe(const message_callback &send) {
	pushPLI(send);
	return true;
}

void RtcpReceivingSession::pushPLI(const message_callback &send) {
	auto message = make_message(RtcpPli::Size(), Message::Control);
	auto *pli = reinterpret_cast<RtcpPli *>(message->data());
	pli->preparePacket(mSsrc);
	send(message);
}

void RtcpReceivingSession::initSeq(uint16_t seq) {
	mBaseSeq = seq;
	mMaxSeq = seq;
	mBadSeq = RTP_SEQ_MOD + 1;   /* so seq == bad_seq is false */
	mCycles = 0;
	mReceived = 0;
	mReceivedPrior = 0;
	mExpectedPrior = 0;
}

bool RtcpReceivingSession::updateSeq(uint16_t seq) {
	uint16_t udelta = seq - mMaxSeq;
	const int MAX_DROPOUT = 3000;
	const int MAX_MISORDER = 100;
	const int MIN_SEQUENTIAL = 2;

	/*
	* Source is not valid until MIN_SEQUENTIAL packets with
	* sequential sequence numbers have been received.
	*/
	if (mProbation) {
		/* packet is in sequence */
		if (seq == mMaxSeq + 1) {
			mProbation--;
			mMaxSeq = seq;
			if (mProbation == 0) {
				initSeq(seq);
				mReceived++;
				return true;
			}
		} else {
			mProbation = MIN_SEQUENTIAL - 1;
			mMaxSeq = seq;
		}
		return false;
	} else if (udelta < MAX_DROPOUT) {
		/* in order, with permissible gap */
		if (seq < mMaxSeq) {
			/*
			* Sequence number wrapped - count another 64K cycle.
			*/
			mCycles += RTP_SEQ_MOD;
		}
		mMaxSeq = seq;
	} else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {
		/* the sequence number made a very large jump */
		if (seq == mBadSeq) {
			/*
			* Two sequential packets -- assume that the other side
			* restarted without telling us so just re-sync
			* (i.e., pretend this was the first packet).
			*/
			initSeq(seq);
		}
		else {
			mBadSeq = (seq + 1) & (RTP_SEQ_MOD-1);
			return false;
		}
	}
	mReceived++;
	return true;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
