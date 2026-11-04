/**
 * Copyright (c) 2025 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "rtc/rtcppauseresumehandler.hpp"
#include "rtc/rtcppauseresumerequester.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr SSRC SENDER_SSRC = 0x12345678;
static constexpr SSRC RECEIVER_SSRC = 0xAABBCCDD;

static message_ptr makeRtpMessage(SSRC ssrc, uint16_t seqNo) {
	auto msg = make_shared<Message>(sizeof(RtpHeader), Message::Binary);
	memset(msg->data(), 0, msg->size());
	auto *rtp = reinterpret_cast<RtpHeader *>(msg->data());
	rtp->_first = 0x80;
	rtp->setSsrc(ssrc);
	rtp->setSeqNumber(seqNo);
	msg->type = Message::Binary;
	return msg;
}

static message_ptr makePauseRtcp(SSRC senderSSRC, SSRC targetSSRC, uint16_t pauseId) {
	auto data = RtcpPauseResume::BuildPause(senderSSRC, targetSSRC, pauseId);
	return make_shared<Message>(data.begin(), data.end(), Message::Control);
}

static message_ptr makeResumeRtcp(SSRC senderSSRC, SSRC targetSSRC, uint16_t pauseId) {
	auto data = RtcpPauseResume::BuildResume(senderSSRC, targetSSRC, pauseId);
	return make_shared<Message>(data.begin(), data.end(), Message::Control);
}

static message_ptr makePausedRtcp(SSRC senderSSRC, SSRC targetSSRC, uint16_t pauseId,
                                  uint32_t extHighSeq) {
	auto data = RtcpPauseResume::BuildPaused(senderSSRC, targetSSRC, pauseId, extHighSeq);
	return make_shared<Message>(data.begin(), data.end(), Message::Control);
}

static message_ptr makeRefusedRtcp(SSRC senderSSRC, SSRC targetSSRC, uint16_t pauseId) {
	auto data = RtcpPauseResume::BuildRefused(senderSSRC, targetSSRC, pauseId);
	return make_shared<Message>(data.begin(), data.end(), Message::Control);
}

struct SentCollector {
	vector<message_ptr> messages;
	message_callback callback() {
		return [this](message_ptr msg) { messages.push_back(msg); };
	}
	void clear() { messages.clear(); }
};

#define REQUIRE(expr)                                                                              \
	do {                                                                                           \
		if (!(expr))                                                                               \
			throw runtime_error(string(__FILE__) + ":" + to_string(__LINE__) + ": " #expr);        \
	} while (0)

#define REQUIRE_EQ(a, b)                                                                           \
	do {                                                                                           \
		if ((a) != (b))                                                                            \
			throw runtime_error(string(__FILE__) + ":" + to_string(__LINE__) + ": " #a " != " #b); \
	} while (0)

// ===========================================================================
// test_pause_resume_packets — packet construction & parsing
// ===========================================================================

TestResult test_pause_resume_packets() {
	try {
		// BuildPause round trip
		{
			SSRC sender = 0x12345678, target = 0xAABBCCDD;
			uint16_t pauseId = 42;
			auto pkt = RtcpPauseResume::BuildPause(sender, target, pauseId);
			REQUIRE_EQ(pkt.size(), RtcpPauseResume::SizeWithFciCount(1));
			auto *pr = reinterpret_cast<const RtcpPauseResume *>(pkt.data());
			REQUIRE_EQ(pr->header.header.payloadType(), RtcpPauseResume::PayloadType);
			REQUIRE_EQ(pr->header.header.reportCount(), RtcpPauseResume::FormatType);
			REQUIRE_EQ(pr->header.packetSenderSSRC(), sender);
			REQUIRE_EQ(pr->header.mediaSourceSSRC(), 0u);
			auto *fci = pr->getFci(0);
			REQUIRE_EQ(fci->targetSsrc(), target);
			REQUIRE_EQ(static_cast<uint8_t>(fci->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Pause));
			REQUIRE_EQ(fci->parameterLen(), 0);
			REQUIRE_EQ(fci->pauseId(), pauseId);
		}

		// BuildResume round trip
		{
			auto pkt = RtcpPauseResume::BuildResume(0x11111111, 0x22222222, 100);
			auto *pr = reinterpret_cast<const RtcpPauseResume *>(pkt.data());
			auto *fci = pr->getFci(0);
			REQUIRE_EQ(static_cast<uint8_t>(fci->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Resume));
			REQUIRE_EQ(fci->pauseId(), 100u);
		}

		// BuildPaused round trip
		{
			uint32_t extHighSeq = 0x0001FFFF;
			auto pkt = RtcpPauseResume::BuildPaused(0xDEADBEEF, 0xCAFEBABE, 7, extHighSeq);
			REQUIRE_EQ(pkt.size(), RtcpPauseResume::SizeWithPausedFci());
			auto *pr = reinterpret_cast<const RtcpPauseResume *>(pkt.data());
			auto *fci = pr->getFci(0);
			REQUIRE_EQ(static_cast<uint8_t>(fci->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Paused));
			REQUIRE_EQ(fci->parameterLen(), 1);
			REQUIRE_EQ(fci->pauseId(), 7u);
			REQUIRE_EQ(fci->extendedHighestSeqNo(), extHighSeq);
			REQUIRE_EQ(fci->getSize(), RtcpPauseResumeFci::BaseSize + 4);
		}

		// BuildRefused round trip
		{
			auto pkt = RtcpPauseResume::BuildRefused(0x99887766, 0x55443322, 0xFFFF);
			auto *pr = reinterpret_cast<const RtcpPauseResume *>(pkt.data());
			auto *fci = pr->getFci(0);
			REQUIRE_EQ(static_cast<uint8_t>(fci->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Refused));
			REQUIRE_EQ(fci->pauseId(), 0xFFFFu);
		}

		// Parse valid packets
		{
			auto pkt = RtcpPauseResume::BuildPause(0x1000, 0x2000, 1);
			auto *parsed = RtcpPauseResume::Parse(pkt.data(), pkt.size());
			REQUIRE(parsed != nullptr);
			REQUIRE_EQ(parsed->header.packetSenderSSRC(), 0x1000u);
		}

		// Parse PAUSED with ext high seq
		{
			auto pkt = RtcpPauseResume::BuildPaused(0x1000, 0x2000, 1, 0xABCD1234);
			auto *parsed = RtcpPauseResume::Parse(pkt.data(), pkt.size());
			REQUIRE(parsed != nullptr);
			REQUIRE_EQ(parsed->getFci(0)->extendedHighestSeqNo(), 0xABCD1234u);
		}

		// Parse too small returns null
		{
			vector<byte> tooSmall(8, byte{0});
			auto *parsed = RtcpPauseResume::Parse(tooSmall.data(), tooSmall.size());
			REQUIRE(parsed == nullptr);
		}

		// Parse wrong payload type returns null
		{
			auto pkt = RtcpPauseResume::BuildPause(0x1000, 0x2000, 1);
			auto *hdr = reinterpret_cast<RtcpPauseResume *>(pkt.data());
			hdr->header.header.setPayloadType(200);
			REQUIRE(RtcpPauseResume::Parse(pkt.data(), pkt.size()) == nullptr);
		}

		// Parse wrong FMT returns null
		{
			auto pkt = RtcpPauseResume::BuildPause(0x1000, 0x2000, 1);
			auto *hdr = reinterpret_cast<RtcpPauseResume *>(pkt.data());
			hdr->header.header.setReportCount(1);
			REQUIRE(RtcpPauseResume::Parse(pkt.data(), pkt.size()) == nullptr);
		}

		// Edge cases: ExtHighSeq zero and max
		{
			auto pkt0 = RtcpPauseResume::BuildPaused(0x1000, 0x2000, 1, 0);
			REQUIRE_EQ(reinterpret_cast<const RtcpPauseResume *>(pkt0.data())
			               ->getFci(0)
			               ->extendedHighestSeqNo(),
			           0u);
			auto pktMax = RtcpPauseResume::BuildPaused(0x1000, 0x2000, 1, 0xFFFFFFFF);
			REQUIRE_EQ(reinterpret_cast<const RtcpPauseResume *>(pktMax.data())
			               ->getFci(0)
			               ->extendedHighestSeqNo(),
			           0xFFFFFFFFu);
		}

		// Packet lengths
		{
			auto pkt = RtcpPauseResume::BuildPause(0x1000, 0x2000, 1);
			auto *pr = reinterpret_cast<const RtcpPauseResume *>(pkt.data());
			REQUIRE_EQ(pr->header.header.length(), 4u);
			REQUIRE_EQ(pr->getSize(), 20u);

			auto pkt2 = RtcpPauseResume::BuildPaused(0x1000, 0x2000, 1, 0x1234);
			auto *pr2 = reinterpret_cast<const RtcpPauseResume *>(pkt2.data());
			REQUIRE_EQ(pr2->header.header.length(), 5u);
			REQUIRE_EQ(pr2->getSize(), 24u);
		}

		// Network byte order verification
		{
			SSRC sender = 0x01020304, target = 0x05060708;
			uint16_t pauseId = 0x0A0B;
			auto pkt = RtcpPauseResume::BuildPause(sender, target, pauseId);
			auto raw = reinterpret_cast<const uint8_t *>(pkt.data());
			REQUIRE_EQ(raw[4], 0x01);
			REQUIRE_EQ(raw[5], 0x02);
			REQUIRE_EQ(raw[6], 0x03);
			REQUIRE_EQ(raw[7], 0x04);
		}

		// Field placement checks
		{
			auto pkt = RtcpPauseResume::BuildPause(0, 0x0A0B0C0D, 0);
			auto raw = reinterpret_cast<const uint8_t *>(pkt.data());
			REQUIRE_EQ(raw[12], 0x0A);
			REQUIRE_EQ(raw[13], 0x0B);
			REQUIRE_EQ(raw[14], 0x0C);
			REQUIRE_EQ(raw[15], 0x0D);
		}

		// Type and param len field placement
		{
			auto pkt = RtcpPauseResume::BuildPause(0, 0, 1);
			auto raw = reinterpret_cast<const uint8_t *>(pkt.data());
			REQUIRE_EQ((raw[16] >> 4) & 0x0F, static_cast<uint8_t>(RtcpPauseResumeType::Pause));
			REQUIRE_EQ(raw[17], 0);

			auto pkt2 = RtcpPauseResume::BuildPaused(0, 0, 1, 0x1234);
			auto raw2 = reinterpret_cast<const uint8_t *>(pkt2.data());
			REQUIRE_EQ((raw2[16] >> 4) & 0x0F, static_cast<uint8_t>(RtcpPauseResumeType::Paused));
			REQUIRE_EQ(raw2[17], 1);
		}

		// PauseID field placement
		{
			auto pkt = RtcpPauseResume::BuildPause(0, 0, 0x0102);
			auto raw = reinterpret_cast<const uint8_t *>(pkt.data());
			REQUIRE_EQ(raw[18], 0x01);
			REQUIRE_EQ(raw[19], 0x02);
		}

		// Byte order: extended highest seq no
		{
			auto pkt = RtcpPauseResume::BuildPaused(0, 0, 1, 0x12345678);
			auto raw = reinterpret_cast<const uint8_t *>(pkt.data());
			REQUIRE_EQ(raw[20], 0x12);
			REQUIRE_EQ(raw[21], 0x34);
			REQUIRE_EQ(raw[22], 0x56);
			REQUIRE_EQ(raw[23], 0x78);
		}

		// Multi-FCI: two PAUSE entries
		{
			auto size = RtcpPauseResume::SizeWithFciCount(2);
			REQUIRE_EQ(size, 28u);
			binary pkt(size, byte{0});
			auto *pr = reinterpret_cast<RtcpPauseResume *>(pkt.data());
			pr->preparePacket(0xAAAAAAAA, 2);
			pr->getFci(0)->setTargetSsrc(0x11111111);
			pr->getFci(0)->setType(RtcpPauseResumeType::Pause);
			pr->getFci(0)->setParameterLen(0);
			pr->getFci(0)->setPauseId(1);
			pr->getFci(1)->setTargetSsrc(0x22222222);
			pr->getFci(1)->setType(RtcpPauseResumeType::Resume);
			pr->getFci(1)->setParameterLen(0);
			pr->getFci(1)->setPauseId(2);

			auto *parsed = RtcpPauseResume::Parse(pkt.data(), pkt.size());
			REQUIRE(parsed != nullptr);
			REQUIRE_EQ(parsed->getFci(0)->targetSsrc(), 0x11111111u);
			REQUIRE_EQ(parsed->getFci(0)->pauseId(), 1u);
			REQUIRE_EQ(parsed->getFci(1)->targetSsrc(), 0x22222222u);
			REQUIRE_EQ(parsed->getFci(1)->pauseId(), 2u);
		}

		// Multi-FCI: three mixed entries
		{
			auto size = RtcpPauseResume::SizeWithFciCount(3);
			REQUIRE_EQ(size, 36u);
			binary pkt(size, byte{0});
			auto *pr = reinterpret_cast<RtcpPauseResume *>(pkt.data());
			pr->preparePacket(0xBBBBBBBB, 3);
			pr->getFci(0)->setTargetSsrc(0x10000001);
			pr->getFci(0)->setType(RtcpPauseResumeType::Pause);
			pr->getFci(0)->setParameterLen(0);
			pr->getFci(0)->setPauseId(100);
			pr->getFci(1)->setTargetSsrc(0x20000002);
			pr->getFci(1)->setType(RtcpPauseResumeType::Resume);
			pr->getFci(1)->setParameterLen(0);
			pr->getFci(1)->setPauseId(200);
			pr->getFci(2)->setTargetSsrc(0x30000003);
			pr->getFci(2)->setType(RtcpPauseResumeType::Refused);
			pr->getFci(2)->setParameterLen(0);
			pr->getFci(2)->setPauseId(300);

			auto *parsed = RtcpPauseResume::Parse(pkt.data(), pkt.size());
			REQUIRE(parsed != nullptr);
			REQUIRE_EQ(parsed->getFci(0)->pauseId(), 100u);
			REQUIRE_EQ(parsed->getFci(1)->pauseId(), 200u);
			REQUIRE_EQ(parsed->getFci(2)->pauseId(), 300u);
		}

		// Multi-FCI packet length
		{
			auto size = RtcpPauseResume::SizeWithFciCount(2);
			binary pkt(size, byte{0});
			auto *pr = reinterpret_cast<RtcpPauseResume *>(pkt.data());
			pr->preparePacket(0, 2);
			REQUIRE_EQ(pr->header.header.length(), 6u);
			REQUIRE_EQ(pr->getSize(), 28u);
		}

		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

// ===========================================================================
// test_pause_resume_handler — sender-side state machine
// ===========================================================================

TestResult test_pause_resume_handler() {
	try {
		// Initial state is Playing with PauseID 0
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC);
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			REQUIRE_EQ(handler->currentPauseId(), 0u);
		}

		// Playing: outgoing RTP passes through
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC);
			message_vector messages;
			messages.push_back(makeRtpMessage(SENDER_SSRC, 100));
			SentCollector sent;
			handler->outgoing(messages, sent.callback());
			REQUIRE_EQ(messages.size(), 1u);
		}

		// Receive PAUSE -> transitions to Paused (nowait)
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 500));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			message_vector incoming;
			incoming.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
			SentCollector sent;
			handler->incoming(incoming, sent.callback());
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Paused));
		}

		// Receive PAUSE -> sends PAUSED indication with correct ext high seq
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 42));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			message_vector incoming;
			incoming.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
			SentCollector sent;
			handler->incoming(incoming, sent.callback());
			REQUIRE(sent.messages.size() >= 1);
			bool foundPaused = false;
			for (auto &msg : sent.messages) {
				auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
				if (pr) {
					auto *fci = pr->getFci(0);
					if (fci->type() == RtcpPauseResumeType::Paused) {
						foundPaused = true;
						REQUIRE_EQ(fci->targetSsrc(), SENDER_SSRC);
						REQUIRE_EQ(fci->pauseId(), 0u);
						REQUIRE_EQ(fci->extendedHighestSeqNo(), 42u);
					}
				}
			}
			REQUIRE(foundPaused);
		}

		// Paused: outgoing RTP suppressed, control passes through
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}

			// RTP suppressed
			message_vector out;
			out.push_back(makeRtpMessage(SENDER_SSRC, 2));
			out.push_back(makeRtpMessage(SENDER_SSRC, 3));
			SentCollector s;
			handler->outgoing(out, s.callback());
			REQUIRE_EQ(out.size(), 0u);

			// Control passes through
			auto ctrlData = RtcpPauseResume::BuildRefused(SENDER_SSRC, RECEIVER_SSRC, 0);
			auto ctrlMsg = make_shared<Message>(ctrlData.begin(), ctrlData.end(), Message::Control);
			message_vector ctrlOut;
			ctrlOut.push_back(ctrlMsg);
			SentCollector s2;
			handler->outgoing(ctrlOut, s2.callback());
			REQUIRE_EQ(ctrlOut.size(), 1u);
		}

		// Receive RESUME -> transitions to Playing, PauseID increments
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			{
				message_vector in;
				in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			REQUIRE_EQ(handler->currentPauseId(), 1u);

			// RTP flows again
			message_vector out;
			out.push_back(makeRtpMessage(SENDER_SSRC, 10));
			SentCollector s;
			handler->outgoing(out, s.callback());
			REQUIRE_EQ(out.size(), 1u);
		}

		// Wrong PauseID -> sends REFUSED, stays Playing
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			{
				message_vector in;
				in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE_EQ(handler->currentPauseId(), 1u);

			SentCollector sent;
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0)); // Stale PauseID
				handler->incoming(in, sent.callback());
			}
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			bool foundRefused = false;
			for (auto &msg : sent.messages) {
				auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
				if (pr && pr->getFci(0)->type() == RtcpPauseResumeType::Refused) {
					foundRefused = true;
					REQUIRE_EQ(pr->getFci(0)->pauseId(), 1u);
				}
			}
			REQUIRE(foundRefused);
		}

		// RFC 7728 Section 8.3: Stale RESUME while Playing should be silently ignored
		// (no REFUSED sent)
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			{
				message_vector in;
				in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE_EQ(handler->currentPauseId(), 1u);
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));

			SentCollector sent;
			{
				message_vector in;
				in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0)); // Stale PauseID
				handler->incoming(in, sent.callback());
			}
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			for (auto &msg : sent.messages) {
				auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
				if (pr) {
					REQUIRE(pr->getFci(0)->type() != RtcpPauseResumeType::Refused);
				}
			}
			REQUIRE_EQ(handler->currentPauseId(), 1u);
		}

		// Local Pause: transitions to LocalPaused, sends PAUSED, gates RTP
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 99));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			SentCollector sent;
			handler->localPause(sent.callback());
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::LocalPaused));

			// PAUSED indication sent
			bool foundPaused = false;
			for (auto &msg : sent.messages) {
				auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
				if (pr && pr->getFci(0)->type() == RtcpPauseResumeType::Paused) {
					foundPaused = true;
					REQUIRE_EQ(pr->getFci(0)->extendedHighestSeqNo(), 99u);
				}
			}
			REQUIRE(foundPaused);

			// RTP gated
			message_vector out;
			out.push_back(makeRtpMessage(SENDER_SSRC, 100));
			SentCollector s;
			handler->outgoing(out, s.callback());
			REQUIRE_EQ(out.size(), 0u);
		}

		// LocalPaused: remote RESUME at the matching PauseID exits (RFC 8853 multi-tier).
		// localPause() marks the LocalPaused state remote-resumable, so the receiver can
		// select a locally-paused stream without the sender calling localResume().
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			SentCollector sent;
			handler->localPause(sent.callback());
			REQUIRE_EQ(handler->currentPauseId(), 0u);
			{
				message_vector in;
				in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
				// Accepted, so no REFUSED goes back
				for (auto &msg : s.messages) {
					auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
					if (pr)
						REQUIRE(pr->getFci(0)->type() != RtcpPauseResumeType::Refused);
				}
			}
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			// PauseID increments on entry to Playing
			REQUIRE_EQ(handler->currentPauseId(), 1u);

			// RTP flows again
			message_vector out;
			out.push_back(makeRtpMessage(SENDER_SSRC, 2));
			SentCollector s;
			handler->outgoing(out, s.callback());
			REQUIRE_EQ(out.size(), 1u);
		}

		// LocalPaused: a stale RESUME (non-current PauseID) is still refused
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			SentCollector sent;
			handler->localPause(sent.callback());
			message_vector in;
			in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 7)); // Stale PauseID
			SentCollector s;
			handler->incoming(in, s.callback());
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::LocalPaused));
			bool foundRefused = false;
			for (auto &msg : s.messages) {
				auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
				if (pr && pr->getFci(0)->type() == RtcpPauseResumeType::Refused) {
					foundRefused = true;
					REQUIRE_EQ(pr->getFci(0)->pauseId(), 0u);
				}
			}
			REQUIRE(foundRefused);
		}

		// localPause() is idempotent: a second call from LocalPaused is a no-op and does
		// not re-emit a PAUSED indication
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			SentCollector first;
			handler->localPause(first.callback());
			REQUIRE_EQ(first.messages.size(), 1u);

			SentCollector second;
			handler->localPause(second.callback());
			REQUIRE_EQ(second.messages.size(), 0u);
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::LocalPaused));
			REQUIRE_EQ(handler->currentPauseId(), 0u);
		}

		// localPause() from remote-Paused does not escalate to LocalPaused or re-emit
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Paused));

			SentCollector sent;
			handler->localPause(sent.callback());
			REQUIRE_EQ(sent.messages.size(), 0u);
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Paused));

			// Still resumable by the remote that paused it
			message_vector in;
			in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
			SentCollector s;
			handler->incoming(in, s.callback());
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
		}

		// setSdpPaused(): enters LocalPaused without a PAUSED indication and without
		// bumping the PauseID (RFC 8853 `~rid` initial pause)
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			int callbackCount = 0;
			RtcpPauseResumeHandler::State lastState = RtcpPauseResumeHandler::State::Playing;
			handler->onStateChanged([&](RtcpPauseResumeHandler::State s) {
				callbackCount++;
				lastState = s;
			});

			handler->setSdpPaused();
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::LocalPaused));
			REQUIRE_EQ(handler->currentPauseId(), 0u);
			REQUIRE_EQ(callbackCount, 1);
			REQUIRE_EQ(static_cast<int>(lastState),
			           static_cast<int>(RtcpPauseResumeHandler::State::LocalPaused));

			// RTP gated from the start
			message_vector out;
			out.push_back(makeRtpMessage(SENDER_SSRC, 1));
			SentCollector s;
			handler->outgoing(out, s.callback());
			REQUIRE_EQ(out.size(), 0u);
			REQUIRE_EQ(s.messages.size(), 0u); // no PAUSED indication
		}

		// setSdpPaused() then remote RESUME at PauseID 0 -> Playing
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			handler->setSdpPaused();

			message_vector in;
			in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
			SentCollector s;
			handler->incoming(in, s.callback());
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			REQUIRE_EQ(handler->currentPauseId(), 1u);

			message_vector out;
			out.push_back(makeRtpMessage(SENDER_SSRC, 1));
			SentCollector s2;
			handler->outgoing(out, s2.callback());
			REQUIRE_EQ(out.size(), 1u);
		}

		// setSdpPaused() from a non-Playing state is a no-op: it must not take over a
		// pause the remote believes it owns
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Paused));

			handler->setSdpPaused();
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Paused));
		}

		// LocalResume -> Playing
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			SentCollector sent;
			handler->localPause(sent.callback());
			handler->localResume();
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
		}

		// localResume() clears the remote-resumable flag: a stale RESUME at the old
		// PauseID after a local resume/re-pause cycle is refused, not accepted
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			SentCollector sent;
			handler->localPause(sent.callback()); // PauseID 0
			handler->localResume();               // -> Playing, PauseID 1
			REQUIRE_EQ(handler->currentPauseId(), 1u);

			message_vector in;
			in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0)); // Stale
			SentCollector s;
			handler->incoming(in, s.callback());
			// Playing + stale RESUME is silently ignored (RFC 7728 Sec 8.3)
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
			REQUIRE_EQ(handler->currentPauseId(), 1u);
		}

		// PAUSE for different SSRC ignored
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			message_vector in;
			in.push_back(makePauseRtcp(RECEIVER_SSRC, 0xDEADDEAD, 0));
			SentCollector sent;
			handler->incoming(in, sent.callback());
			REQUIRE_EQ(static_cast<int>(handler->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));
		}

		// Multiple cycles: PauseID increments correctly
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			for (uint16_t i = 0; i < 3; i++) {
				{
					message_vector out;
					out.push_back(makeRtpMessage(SENDER_SSRC, i * 10));
					SentCollector s;
					handler->outgoing(out, s.callback());
				}
				{
					message_vector in;
					in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, i));
					SentCollector s;
					handler->incoming(in, s.callback());
				}
				REQUIRE_EQ(handler->currentPauseId(), i);
				{
					message_vector in;
					in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, i));
					SentCollector s;
					handler->incoming(in, s.callback());
				}
				REQUIRE_EQ(handler->currentPauseId(), static_cast<uint16_t>(i + 1));
			}
		}

		// Highest seq no tracks outgoing RTP (including out-of-order)
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 10));
				out.push_back(makeRtpMessage(SENDER_SSRC, 20));
				out.push_back(makeRtpMessage(SENDER_SSRC, 15));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			SentCollector sent;
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				handler->incoming(in, sent.callback());
			}
			bool foundPaused = false;
			for (auto &msg : sent.messages) {
				auto *pr = RtcpPauseResume::Parse(msg->data(), msg->size());
				if (pr && pr->getFci(0)->type() == RtcpPauseResumeType::Paused) {
					foundPaused = true;
					REQUIRE_EQ(pr->getFci(0)->extendedHighestSeqNo(), 20u);
				}
			}
			REQUIRE(foundPaused);
		}

		// Non-PAUSE-RESUME RTCP passes through
		{
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			auto msg = make_shared<Message>(28, Message::Control);
			memset(msg->data(), 0, msg->size());
			reinterpret_cast<RtcpHeader *>(msg->data())->setPayloadType(200);
			message_vector in;
			in.push_back(msg);
			SentCollector sent;
			handler->incoming(in, sent.callback());
			REQUIRE_EQ(in.size(), 1u);
		}

		// State change callbacks
		{
			bool pausedCb = false, resumedCb = false;
			auto handler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			handler->onStateChanged([&](RtcpPauseResumeHandler::State s) {
				if (s == RtcpPauseResumeHandler::State::Paused)
					pausedCb = true;
				if (s == RtcpPauseResumeHandler::State::Playing)
					resumedCb = true;
			});
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 1));
				SentCollector s;
				handler->outgoing(out, s.callback());
			}
			{
				message_vector in;
				in.push_back(makePauseRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE(pausedCb);
			resumedCb = false;
			{
				message_vector in;
				in.push_back(makeResumeRtcp(RECEIVER_SSRC, SENDER_SSRC, 0));
				SentCollector s;
				handler->incoming(in, s.callback());
			}
			REQUIRE(resumedCb);
		}

		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

// ===========================================================================
// test_pause_resume_requester — receiver-side logic
// ===========================================================================

TestResult test_pause_resume_requester() {
	try {
		// Request PAUSE sends packet and sets state
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			SentCollector sent;
			req->requestPause(SENDER_SSRC, sent.callback());
			REQUIRE_EQ(sent.messages.size(), 1u);
			auto *pr = RtcpPauseResume::Parse(sent.messages[0]->data(), sent.messages[0]->size());
			REQUIRE(pr != nullptr);
			auto *fci = pr->getFci(0);
			REQUIRE_EQ(fci->targetSsrc(), SENDER_SSRC);
			REQUIRE_EQ(static_cast<uint8_t>(fci->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Pause));
			REQUIRE_EQ(fci->pauseId(), 0u);
			REQUIRE_EQ(static_cast<int>(req->streamState(SENDER_SSRC)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::PauseRequested));
		}

		// Request RESUME after PAUSED
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			SentCollector sent;
			req->requestPause(SENDER_SSRC, sent.callback());
			{
				message_vector in;
				in.push_back(makePausedRtcp(SENDER_SSRC, SENDER_SSRC, 0, 100));
				req->incoming(in, sent.callback());
			}
			sent.clear();
			req->requestResume(SENDER_SSRC, sent.callback());
			REQUIRE_EQ(sent.messages.size(), 1u);
			auto *pr = RtcpPauseResume::Parse(sent.messages[0]->data(), sent.messages[0]->size());
			REQUIRE(pr != nullptr);
			REQUIRE_EQ(static_cast<uint8_t>(pr->getFci(0)->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Resume));
		}

		// Receive PAUSED updates state and fires callback
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			SSRC cbSsrc = 0;
			uint16_t cbPauseId = 0xFFFF;
			uint32_t cbExtHighSeq = 0;
			req->onStreamPaused([&](SSRC ssrc, uint16_t pauseId, uint32_t extHighSeq) {
				cbSsrc = ssrc;
				cbPauseId = pauseId;
				cbExtHighSeq = extHighSeq;
			});
			SentCollector sent;
			req->requestPause(SENDER_SSRC, sent.callback());
			{
				message_vector in;
				in.push_back(makePausedRtcp(SENDER_SSRC, SENDER_SSRC, 0, 12345));
				req->incoming(in, sent.callback());
			}
			REQUIRE_EQ(static_cast<int>(req->streamState(SENDER_SSRC)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Paused));
			REQUIRE_EQ(cbSsrc, SENDER_SSRC);
			REQUIRE_EQ(cbPauseId, 0u);
			REQUIRE_EQ(cbExtHighSeq, 12345u);
		}

		// Unsolicited PAUSED fires callback
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			bool cbFired = false;
			req->onStreamPaused([&](SSRC, uint16_t, uint32_t) { cbFired = true; });
			message_vector in;
			in.push_back(makePausedRtcp(SENDER_SSRC, SENDER_SSRC, 0, 0));
			SentCollector sent;
			req->incoming(in, sent.callback());
			REQUIRE(cbFired);
		}

		// Receive REFUSED fires callback, updates PauseID, back to Playing
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			SSRC cbSsrc = 0;
			uint16_t cbCorrectPauseId = 0xFFFF;
			req->onStreamRefused([&](SSRC ssrc, uint16_t correctPauseId) {
				cbSsrc = ssrc;
				cbCorrectPauseId = correctPauseId;
			});
			SentCollector sent;
			req->requestPause(SENDER_SSRC, sent.callback());
			{
				message_vector in;
				in.push_back(makeRefusedRtcp(SENDER_SSRC, SENDER_SSRC, 5));
				req->incoming(in, sent.callback());
			}
			REQUIRE_EQ(cbSsrc, SENDER_SSRC);
			REQUIRE_EQ(cbCorrectPauseId, 5u);
			REQUIRE_EQ(req->trackedPauseId(SENDER_SSRC), 5u);
			REQUIRE_EQ(static_cast<int>(req->streamState(SENDER_SSRC)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Playing));
		}

		// PauseID tracking across resume cycle
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			SentCollector sent;
			req->requestPause(SENDER_SSRC, sent.callback());
			{
				message_vector in;
				in.push_back(makePausedRtcp(SENDER_SSRC, SENDER_SSRC, 0, 100));
				req->incoming(in, sent.callback());
			}
			REQUIRE_EQ(req->trackedPauseId(SENDER_SSRC), 0u);
			sent.clear();
			req->requestResume(SENDER_SSRC, sent.callback());
			REQUIRE(sent.messages.size() >= 1);
			auto *pr = RtcpPauseResume::Parse(sent.messages[0]->data(), sent.messages[0]->size());
			REQUIRE(pr != nullptr);
			REQUIRE_EQ(pr->getFci(0)->pauseId(), 0u);
		}

		// Unknown SSRC returns Playing
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			REQUIRE_EQ(static_cast<int>(req->streamState(0xDEADBEEF)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Playing));
		}

		// Non-pause-resume RTCP passes through
		{
			auto req = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			auto msg = make_shared<Message>(28, Message::Control);
			memset(msg->data(), 0, msg->size());
			reinterpret_cast<RtcpHeader *>(msg->data())->setPayloadType(200);
			message_vector in;
			in.push_back(msg);
			SentCollector sent;
			req->incoming(in, sent.callback());
			REQUIRE_EQ(in.size(), 1u);
		}

		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

// ===========================================================================
// test_pause_resume_chain — end-to-end integration
// ===========================================================================

TestResult test_pause_resume_chain() {
	try {
		// End-to-end: Receiver PAUSE -> Sender PAUSED indication
		{
			auto sender = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			auto receiver = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);

			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 500));
				SentCollector s;
				sender->outgoing(out, s.callback());
			}

			SentCollector receiverSent;
			receiver->requestPause(SENDER_SSRC, receiverSent.callback());
			REQUIRE_EQ(receiverSent.messages.size(), 1u);

			SentCollector senderSent;
			{
				message_vector in;
				in.push_back(receiverSent.messages[0]);
				sender->incoming(in, senderSent.callback());
			}
			REQUIRE_EQ(static_cast<int>(sender->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Paused));
			REQUIRE(senderSent.messages.size() >= 1);

			{
				message_vector in;
				in.push_back(senderSent.messages[0]);
				SentCollector s;
				receiver->incoming(in, s.callback());
			}
			REQUIRE_EQ(static_cast<int>(receiver->streamState(SENDER_SSRC)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Paused));
		}

		// Full PAUSE/RESUME cycle with callbacks, RTP gating, and PauseID increment
		{
			auto sender = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			auto receiver = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			bool senderPaused = false, senderResumed = false, receiverPaused = false;

			sender->onStateChanged([&](RtcpPauseResumeHandler::State s) {
				if (s == RtcpPauseResumeHandler::State::Paused)
					senderPaused = true;
				if (s == RtcpPauseResumeHandler::State::Playing)
					senderResumed = true;
			});
			receiver->onStreamPaused([&](SSRC, uint16_t, uint32_t) { receiverPaused = true; });

			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 100));
				SentCollector s;
				sender->outgoing(out, s.callback());
			}

			// PAUSE
			SentCollector receiverSent;
			receiver->requestPause(SENDER_SSRC, receiverSent.callback());
			SentCollector senderSent;
			{
				message_vector in;
				in.push_back(receiverSent.messages[0]);
				sender->incoming(in, senderSent.callback());
			}
			REQUIRE(senderPaused);
			{
				message_vector in;
				in.push_back(senderSent.messages[0]);
				SentCollector s;
				receiver->incoming(in, s.callback());
			}
			REQUIRE(receiverPaused);

			// RTP gated
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 101));
				SentCollector s;
				sender->outgoing(out, s.callback());
				REQUIRE_EQ(out.size(), 0u);
			}

			// RESUME
			receiverSent.clear();
			receiver->requestResume(SENDER_SSRC, receiverSent.callback());
			senderSent.clear();
			{
				message_vector in;
				in.push_back(receiverSent.messages[0]);
				sender->incoming(in, senderSent.callback());
			}
			REQUIRE(senderResumed);
			REQUIRE_EQ(static_cast<int>(sender->state()),
			           static_cast<int>(RtcpPauseResumeHandler::State::Playing));

			// RTP flows again
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 102));
				SentCollector s;
				sender->outgoing(out, s.callback());
				REQUIRE_EQ(out.size(), 1u);
			}
			REQUIRE_EQ(sender->currentPauseId(), 1u);
		}

		// Chain API: pauseStream/resumeStream via MediaHandler base
		{
			auto dummyHandler = make_shared<MediaHandler>();
			auto requester = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			dummyHandler->addToChain(requester);

			SentCollector sent;
			bool result = dummyHandler->pauseStream(SENDER_SSRC, sent.callback());
			REQUIRE(result);
			REQUIRE_EQ(sent.messages.size(), 1u);
			auto *pr = RtcpPauseResume::Parse(sent.messages[0]->data(), sent.messages[0]->size());
			REQUIRE(pr != nullptr);
			REQUIRE_EQ(static_cast<uint8_t>(pr->getFci(0)->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Pause));
			REQUIRE_EQ(pr->getFci(0)->targetSsrc(), SENDER_SSRC);

			// Feed PAUSED back
			{
				auto pausedData = RtcpPauseResume::BuildPaused(SENDER_SSRC, SENDER_SSRC, 0, 50);
				auto pausedMsg =
				    make_shared<Message>(pausedData.begin(), pausedData.end(), Message::Control);
				message_vector in;
				in.push_back(pausedMsg);
				requester->incoming(in, sent.callback());
			}

			// Resume via chain
			sent.clear();
			result = dummyHandler->resumeStream(SENDER_SSRC, sent.callback());
			REQUIRE(result);
			REQUIRE_EQ(sent.messages.size(), 1u);
			pr = RtcpPauseResume::Parse(sent.messages[0]->data(), sent.messages[0]->size());
			REQUIRE(pr != nullptr);
			REQUIRE_EQ(static_cast<uint8_t>(pr->getFci(0)->type()),
			           static_cast<uint8_t>(RtcpPauseResumeType::Resume));
		}

		// Chain API: onStreamPaused callback via chain
		{
			auto dummyHandler = make_shared<MediaHandler>();
			auto requester = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			dummyHandler->addToChain(requester);

			bool cbFired = false;
			uint32_t cbExtHighSeq = 0;
			dummyHandler->onStreamPaused([&](uint32_t, uint16_t, uint32_t extHighSeq) {
				cbFired = true;
				cbExtHighSeq = extHighSeq;
			});

			auto pausedData = RtcpPauseResume::BuildPaused(SENDER_SSRC, SENDER_SSRC, 0, 999);
			auto pausedMsg =
			    make_shared<Message>(pausedData.begin(), pausedData.end(), Message::Control);
			message_vector in;
			in.push_back(pausedMsg);
			SentCollector sent;
			requester->incoming(in, sent.callback());
			REQUIRE(cbFired);
			REQUIRE_EQ(cbExtHighSeq, 999u);
		}

		// No requester in chain -> pauseStream returns false
		{
			auto dummyHandler = make_shared<MediaHandler>();
			SentCollector sent;
			REQUIRE(!dummyHandler->pauseStream(SENDER_SSRC, sent.callback()));
			REQUIRE_EQ(sent.messages.size(), 0u);
		}

		// Sender handler in chain: RTP gated when paused
		{
			auto dummyHandler = make_shared<MediaHandler>();
			auto senderHandler = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			dummyHandler->addToChain(senderHandler);

			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 50));
				SentCollector s;
				dummyHandler->outgoingChain(out, s.callback());
				REQUIRE_EQ(out.size(), 1u);
			}
			{
				auto pauseData = RtcpPauseResume::BuildPause(RECEIVER_SSRC, SENDER_SSRC, 0);
				auto pauseMsg =
				    make_shared<Message>(pauseData.begin(), pauseData.end(), Message::Control);
				message_vector in;
				in.push_back(pauseMsg);
				SentCollector s;
				senderHandler->incoming(in, s.callback());
			}
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 51));
				SentCollector s;
				dummyHandler->outgoingChain(out, s.callback());
				REQUIRE_EQ(out.size(), 0u);
			}
		}

		// Multiple SSRCs are independent
		{
			auto requester = make_shared<RtcpPauseResumeRequester>(RECEIVER_SSRC);
			SSRC videoSsrc = 0x11111111, audioSsrc = 0x22222222;
			SentCollector sent;
			requester->requestPause(videoSsrc, sent.callback());
			REQUIRE_EQ(static_cast<int>(requester->streamState(videoSsrc)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::PauseRequested));
			REQUIRE_EQ(static_cast<int>(requester->streamState(audioSsrc)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Playing));

			{
				auto pausedData = RtcpPauseResume::BuildPaused(videoSsrc, videoSsrc, 0, 100);
				auto msg =
				    make_shared<Message>(pausedData.begin(), pausedData.end(), Message::Control);
				message_vector in;
				in.push_back(msg);
				requester->incoming(in, sent.callback());
			}
			REQUIRE_EQ(static_cast<int>(requester->streamState(videoSsrc)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Paused));
			REQUIRE_EQ(static_cast<int>(requester->streamState(audioSsrc)),
			           static_cast<int>(RtcpPauseResumeRequester::StreamState::Playing));
		}

		// PAUSED extended highest seq no correctness
		{
			auto sender = make_shared<RtcpPauseResumeHandler>(SENDER_SSRC, true);
			{
				message_vector out;
				out.push_back(makeRtpMessage(SENDER_SSRC, 100));
				out.push_back(makeRtpMessage(SENDER_SSRC, 200));
				out.push_back(makeRtpMessage(SENDER_SSRC, 300));
				SentCollector s;
				sender->outgoing(out, s.callback());
			}
			SentCollector senderSent;
			{
				auto pauseData = RtcpPauseResume::BuildPause(RECEIVER_SSRC, SENDER_SSRC, 0);
				auto msg =
				    make_shared<Message>(pauseData.begin(), pauseData.end(), Message::Control);
				message_vector in;
				in.push_back(msg);
				sender->incoming(in, senderSent.callback());
			}
			REQUIRE(senderSent.messages.size() >= 1);
			auto *pr = RtcpPauseResume::Parse(senderSent.messages[0]->data(),
			                                  senderSent.messages[0]->size());
			REQUIRE(pr != nullptr);
			REQUIRE_EQ(pr->getFci(0)->extendedHighestSeqNo(), 300u);
		}

		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

// PauseResume SDP syntax tests
TestResult test_pause_resume_sdp() {
	InitLogger(LogLevel::Debug);

	// Build a description with one video and one audio media
	Description desc("v=0\r\n"
	                 "o=- 0 0 IN IP4 0.0.0.0\r\n"
	                 "s=-\r\n"
	                 "t=0 0\r\n",
	                 Description::Type::Offer);

	// Start with just an audio track that does not have pause resume
	Description::Audio audio("audio", Description::Direction::SendOnly);
	audio.addOpusCodec(111);
	desc.addMedia(audio);

	string sdp1 = desc.generateSdp();
	if (sdp1.find("ccm pause") != string::npos || audio.isPauseResumeEnabled()) {
		return TestResult(false, "Pause / Resume entry found when not explicitly added");
	}

	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(96);
	video.addVP8Codec(97);
	video.addPauseResume();
	desc.addMedia(video);

	sdp1 = desc.generateSdp();
	if (sdp1.find("ccm pause") == string::npos || !video.isPauseResumeEnabled())
		return TestResult(false, "Pause / Resume entry not found in Offer");

	if (sdp1.find("ccm pause nowait") == string::npos)
		return TestResult(false,
		                  "Pause / Resume entry not found in Offer with incorrect default values");

	if (sdp1.find(" config=") != string::npos)
		return TestResult(
		    false, "Pause / Resume config entry found in Offer with incorrect default values");

	desc = Description("v=0\r\n"
	                   "o=- 0 0 IN IP4 0.0.0.0\r\n"
	                   "s=-\r\n"
	                   "t=0 0\r\n",
	                   Description::Type::Offer);
	video = Description::Video("video", Description::Direction::SendOnly);
	video.addH264Codec(98);
	video.addVP8Codec(99);
	video.addPauseResume(false, 1);
	desc.addMedia(video);
	sdp1 = desc.generateSdp();

	if (!video.isPauseResumeEnabled())
		return TestResult(false, "Pause / Resume incorrectly saying disabled");

	if (sdp1.find(" nowait") != string::npos)
		return TestResult(false,
		                  "Pause / Resume nowait entry found in Offer when it should not be there");

	if (sdp1.find(" config=1") == string::npos)
		return TestResult(false, "Pause / Resume config entry not found");

	return TestResult(true);
}

// Negotiation tests: Test various combos of Offer / Answer

// PauseResume negotiation via PeerConnection — offer has it, answer does too
TestResult test_pause_resume_offer_yes_answer_yes() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(string(sdp)); });

	pc1.onLocalCandidate(
	    [&pc2](Candidate candidate) { pc2.addRemoteCandidate(string(candidate)); });

	// Intercept pc2's answer: strip pause before forwarding to pc1
	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });

	pc2.onLocalCandidate(
	    [&pc1](Candidate candidate) { pc1.addRemoteCandidate(string(candidate)); });

	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(96);
	video.addVP8Codec(97);
	video.addPauseResume(); // Should implicitly add nowait and no config.

	auto t1 = pc1.addTrack(video);

	if (!t1->description().isPauseResumeEnabled())
		return TestResult(false, "Offerer track should have pause enabled before negotiation");

	shared_ptr<Track> t2;
	pc2.onTrack([&t2](shared_ptr<Track> t) { std::atomic_store(&t2, t); });

	pc1.setLocalDescription();

	// Wait for connection
	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	// The offerer should have pause resume disabled after receiving the answer without pause resume
	auto m0 = pc1.remoteDescription()->media(0);
	auto *vid = std::get_if<Description::Media *>(&m0);
	if (!vid || !(*vid)->isPauseResumeEnabled())
		return TestResult(
		    false, "Offerer track should have pause/resume enabled after answer includes support");

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}

// PauseResume negotiation via PeerConnection — offerer has it, answer does not
TestResult test_pause_resume_offer_yes_answer_no() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(string(sdp)); });

	pc1.onLocalCandidate(
	    [&pc2](Candidate candidate) { pc2.addRemoteCandidate(string(candidate)); });

	// Intercept pc2's answer: strip pause before forwarding to pc1
	pc2.onLocalDescription([&pc1](Description sdp) {
		for (int i = 0; i < sdp.mediaCount(); ++i) {
			auto media = sdp.media(i);
			if (auto *m = std::get_if<Description::Media *>(&media))
				(*m)->removePauseResume();
		}
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate(
	    [&pc1](Candidate candidate) { pc1.addRemoteCandidate(string(candidate)); });

	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(96);
	video.addVP8Codec(97);
	video.addPauseResume();

	auto t1 = pc1.addTrack(video);

	if (!t1->description().isPauseResumeEnabled())
		return TestResult(false, "Offerer track should have pause enabled before negotiation");

	shared_ptr<Track> t2;
	pc2.onTrack([&t2](shared_ptr<Track> t) { std::atomic_store(&t2, t); });

	pc1.setLocalDescription();

	// Wait for connection
	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	// The answer should have pause resume disabled
	auto m0 = pc1.remoteDescription()->media(0);
	auto *vid = std::get_if<Description::Media *>(&m0);
	cerr << "Remote Description:" << endl << pc1.remoteDescription()->generateSdp() << endl;
	if (!vid || (*vid)->isPauseResumeEnabled()) {
		return TestResult(false, "Answer should have pause/resume disabled");
	}

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}

#endif // RTC_ENABLE_MEDIA
