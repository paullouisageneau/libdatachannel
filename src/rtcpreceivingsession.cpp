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
#include <mutex>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

static impl::LogCounter COUNTER_BAD_RTP_HEADER(plog::warning, "Number of malformed RTP headers");
static impl::LogCounter COUNTER_BAD_RTCP_HEADER(plog::warning, "Number of malformed RTCP headers");
static impl::LogCounter COUNTER_UNKNOWN_PPID(plog::warning, "Number of Unknown PPID messages");
static impl::LogCounter COUNTER_BAD_NOTIF_LEN(plog::warning,
                                              "Number of Bad-Lengthed notifications");
static impl::LogCounter COUNTER_BAD_SCTP_STATUS(plog::warning,
                                                "Number of unknown SCTP_STATUS errors");

RtcpReceivingSession::SyncTimestamps RtcpReceivingSession::getSyncTimestamps(){
	std::lock_guard lock(mSyncMutex);
	return mSyncTimestamps;
}

void RtcpReceivingSession::media(const Description::Media &desc) {
	bool newRtxEnabled = false;
	std::unordered_map<uint8_t, uint8_t> newRtxToPrimaryPtMap;
	SSRC newRtxPrimarySsrc = 0;

	if (desc.isRtxEnabled()) {
		auto pts = desc.payloadTypes();

		auto ssrcs = desc.getSSRCs();
		for (auto ssrc : ssrcs) {
			auto rtxSsrc = desc.getRtxSsrcForSsrc(ssrc);
			if (rtxSsrc) {
				newRtxPrimarySsrc = ssrc;
				break;
			}
		}

		// Build mapping from each RTX PT to its primary PT
		for (int pt : pts) {
			auto rtxPt = desc.getRtxPayloadType(pt);
			if (rtxPt) {
				newRtxToPrimaryPtMap[static_cast<uint8_t>(*rtxPt)] =
				    static_cast<uint8_t>(pt);
			}
		}

		// Enable RTX if PT mapping is not empty
		newRtxEnabled = !newRtxToPrimaryPtMap.empty();
	}

	std::lock_guard lock(mMutex);
	mRtxEnabled = newRtxEnabled;
	mRtxToPrimaryPtMap = std::move(newRtxToPrimaryPtMap);
	mRtxPrimarySsrc = newRtxPrimarySsrc;
}

message_ptr RtcpReceivingSession::unwrapRtx(const message_ptr &rtxPacket) {
	if (!rtxPacket || rtxPacket->size() < sizeof(RtpHeader) + sizeof(uint16_t))
		return nullptr;

	auto rtxRtp = reinterpret_cast<const RtpHeader *>(rtxPacket->data());
	uint8_t rtxPt = rtxRtp->payloadType();

	SSRC primarySsrc;
	uint8_t primaryPayloadType;
	{
		std::lock_guard lock(mMutex);
		primarySsrc = mRtxPrimarySsrc;
		// If primary SSRC not provided in SDP and first primary packet not arrived return nullptr
		if (primarySsrc == 0)
			return nullptr;
		auto it = mRtxToPrimaryPtMap.find(rtxPt);
		if (it == mRtxToPrimaryPtMap.end())
			return nullptr;
		primaryPayloadType = it->second;
	}

	size_t totalSize = rtxPacket->size();

	// Allocate a new message to prevent corruption
	auto unwrapped = make_message(totalSize, rtxPacket);

	auto rtx = reinterpret_cast<RtpRtx *>(unwrapped->data());
	size_t newSize = rtx->normalizePacket(totalSize, primarySsrc, primaryPayloadType);

	unwrapped->resize(newSize);
	unwrapped->stream = primarySsrc;
	return unwrapped;
}

void RtcpReceivingSession::incoming(message_vector &messages, const message_callback &send) {
	// Unwrap RTX packets before processing
	bool rtxEnabled;
	std::unordered_map<uint8_t, uint8_t> rtxToPrimaryPtMap;
	{
		std::lock_guard lock(mMutex);
		rtxEnabled = mRtxEnabled;
		rtxToPrimaryPtMap = mRtxToPrimaryPtMap;
	}

	if (rtxEnabled) {
		for (auto &message : messages) {
			if (message->type == Message::Control)
				continue;

			if (message->size() < sizeof(RtpHeader))
				continue;

			auto rtp = reinterpret_cast<const RtpHeader *>(message->data());
			uint8_t pt = rtp->payloadType();

			if (rtxToPrimaryPtMap.count(pt)) {
				// RTX packet
				auto unwrapped = unwrapRtx(message);
				if (unwrapped)
					message = unwrapped;
			} else {
				// Primary packet
				std::lock_guard lock(mMutex);
				// if mRtxPrimarySsrc was not provided in SDP set it
				if (mRtxPrimarySsrc == 0)
					mRtxPrimarySsrc = rtp->ssrc();
			}
		}
	}

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
			if (message->size() < sizeof(RtcpHeader)) {
				COUNTER_BAD_RTCP_HEADER++;
				PLOG_VERBOSE << "RTCP packet is too small, size=" << message->size();
				continue;
			}
			auto header = reinterpret_cast<const RtcpHeader *>(message->data());
			if (header->payloadType() == 201) { // RR
				if (message->size() < RtcpRr::SizeWithReportBlocks(0)) {
					COUNTER_BAD_RTCP_HEADER++;
					PLOG_VERBOSE << "RTCP RR is too small, size=" << message->size();
					continue;
				}
				auto rr = reinterpret_cast<const RtcpRr *>(message->data());
				mSsrc = rr->senderSSRC();
				rr->log();
			} else if (header->payloadType() == 200) { // SR
				if (message->size() < RtcpSr::Size(0)) {
					COUNTER_BAD_RTCP_HEADER++;
					PLOG_VERBOSE << "RTCP SR is too small, size=" << message->size();
					continue;
				}
				auto sr = reinterpret_cast<const RtcpSr *>(message->data());
				mSsrc = sr->senderSSRC();
				{
					std::lock_guard lock(mSyncMutex);
					mSyncTimestamps.rtpTimestamp = sr->rtpTimestamp();
					mSyncTimestamps.ntpTimestamp = sr->ntpTimestamp();
				}
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
	remb->setSSRC(0, mSsrc);
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
	auto syncTimestamps = getSyncTimestamps();
	auto reportBlock = rr->getReportBlock(0);
	assert(reportBlock);
	reportBlock->preparePacket(mSsrc, fraction, lost, uint16_t(mGreatestSeqNo), mMaxSeq, 0, syncTimestamps.ntpTimestamp,
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
