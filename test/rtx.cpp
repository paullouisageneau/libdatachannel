/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace rtc;
using namespace std;

static const SSRC PRIMARY_SSRC = 1234;
static const SSRC RTX_SSRC = 5678;
static const uint8_t PRIMARY_PT = 96;
static const uint8_t RTX_PT = 97;
static const uint32_t CLOCK_RATE = 90000;
static const uint32_t BITRATE = 3000;
static const uint16_t PORT_RANGE_BEGIN = 5000;
static const uint16_t PORT_RANGE_END = 6000;
static const char *CNAME = "video-send";

// Helper MediaHandler NACK generator for use in tests
// Tracks the highest received sequence number and sends a NACK for any gap
class TestNackGenerator final : public MediaHandler {
public:
	void incoming(message_vector &messages, const message_callback &send) override {
		for (auto &msg : messages) {
			if (msg->type == Message::Control || msg->size() < sizeof(RtpHeader))
				continue;

			auto *rtp = reinterpret_cast<const RtpHeader *>(msg->data());
			uint16_t seq = rtp->seqNumber();
			SSRC ssrc = rtp->ssrc();

			if (!mInitialized) {
				mMaxSeq = seq;
				mInitialized = true;
				continue;
			}

			uint16_t udelta = seq - mMaxSeq;
			if (udelta == 0 || udelta >= 3000)
				continue; // duplicate or reorder/wrap — skip

			// Send a NACK for each missing sequence number in the gap
			for (uint16_t delta = 1; delta < udelta; ++delta) {
				uint16_t missingSeq = mMaxSeq + delta;
				auto message = make_message(RtcpNack::Size(1), Message::Control);
				auto *nack = reinterpret_cast<RtcpNack *>(message->data());
				nack->preparePacket(ssrc, 1);
				unsigned int fciCount = 0;
				uint16_t fciPID = 0;
				nack->addMissingPacket(&fciCount, &fciPID, missingSeq);
				send(message);
			}

			mMaxSeq = seq;
		}
	}

private:
	uint16_t mMaxSeq = 0;
	bool mInitialized = false;
};

// Helper MediaHandler that intercepts raw RTP packets on the wire BEFORE RTX unwrapping.
class RtxWireInterceptor final : public MediaHandler {
public:
	RtxWireInterceptor(SSRC rtxSsrc, uint8_t rtxPt) : mRtxSsrc(rtxSsrc), mRtxPt(rtxPt) {}

	void incoming(message_vector &messages, const message_callback &send) override {
		for (auto &msg : messages) {
			if (msg->type == Message::Control || msg->size() < sizeof(RtpHeader))
				continue;
			auto *rtp = reinterpret_cast<const RtpHeader *>(msg->data());
			if (rtp->ssrc() == mRtxSsrc && rtp->payloadType() == mRtxPt) {
				cout << "RtxWireInterceptor: saw RTX wire packet SSRC=" << rtp->ssrc()
				     << " PT=" << (int)rtp->payloadType()
				     << " seq=" << rtp->seqNumber() << endl;
				mRtxPacketsSeen++;
			}
		}
	}

	std::atomic<int> mRtxPacketsSeen{0};

private:
	SSRC mRtxSsrc;
	uint8_t mRtxPt;
};

// Helper MediaHandler that drops RTP packets matching target sequence numbers
class PacketDropper final : public MediaHandler {
public:
	explicit PacketDropper(std::unordered_set<uint16_t> dropSeqNos) : mDropSeqNos(std::move(dropSeqNos)) {}

	void incoming(message_vector &messages, const message_callback &send) override {
		for (auto it = messages.begin(); it != messages.end();) {
			auto &msg = *it;
			if (msg->type != Message::Control && msg->size() >= sizeof(RtpHeader)) {
				auto *rtp = reinterpret_cast<const RtpHeader *>(msg->data());
				if (mDropSeqNos.count(rtp->seqNumber())) {
					cout << "PacketDropper: dropping packet seq=" << rtp->seqNumber() << endl;
					it = messages.erase(it);
					continue;
				}
			}
			++it;
		}
	}

private:
	std::unordered_set<uint16_t> mDropSeqNos;
};

// End-to-end test: sender transmits multiple RTP packets, receiver detects a gap,
// sends a NACK, and verifies the RTX retransmission recovers the dropped packet
TestResult test_rtx_dropped_packet() {
	InitLogger(LogLevel::Debug);
	cout << "End to End Dropped Packet Test" << endl;

	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = PORT_RANGE_BEGIN;
	config2.portRangeEnd = PORT_RANGE_END;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		cout << "Description 1: " << sdp << endl;
		pc2.setRemoteDescription(string(sdp));
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		cout << "Candidate 1: " << candidate << endl;
		pc2.addRemoteCandidate(string(candidate));
	});

	pc1.onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });

	pc1.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
	});

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		cout << "Candidate 2: " << candidate << endl;
		pc1.addRemoteCandidate(string(candidate));
	});

	pc2.onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });

	pc2.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	// Set up sender track with RTX
	Description::Video media("video", Description::Direction::SendOnly);
	media.addH264Codec(PRIMARY_PT);
	media.addRtxCodec(RTX_PT, PRIMARY_PT, CLOCK_RATE);
	media.setBitrate(BITRATE);
	media.addSSRC(PRIMARY_SSRC, CNAME);
	media.addRtxSSRC(PRIMARY_SSRC, RTX_SSRC);

	shared_ptr<Track> t2;
	shared_ptr<RtxWireInterceptor> capturedRtxInterceptor;
	promise<rtc::binary> recvPromiseSeq2;
	promise<rtc::binary> recvPromiseSeq4;
	const std::unordered_set<uint16_t> dropSeqs = {2, 4};

	pc2.onTrack([&t2, &capturedRtxInterceptor, &recvPromiseSeq2, &recvPromiseSeq4, &dropSeqs](shared_ptr<Track> t) {
		string mid = t->mid();
		cout << "Track 2: Received track with mid \"" << mid << "\"" << endl;

		t->onOpen([mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is open" << endl; });

		t->onClosed(
		    [mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is closed" << endl; });

		t->onMessage(
		    [&recvPromiseSeq2, &recvPromiseSeq4](rtc::binary message) {
			    if (IsRtcp(message))
				    return;
			    if (message.size() < sizeof(RtpHeader))
				    return;
			    auto *rtp = reinterpret_cast<const RtpHeader *>(message.data());
			    uint16_t seq = rtp->seqNumber();
			    if (seq == 2)
				    recvPromiseSeq2.set_value(std::move(message));
			    else if (seq == 4)
				    recvPromiseSeq4.set_value(std::move(message));
		    },
		    nullptr);

		auto desc = t->description();
		desc.addSSRC(PRIMARY_SSRC, CNAME);
		desc.addRtxSSRC(PRIMARY_SSRC, RTX_SSRC);
		t->setDescription(desc);

		// Add PacketDropper to chain to drop seq=2 and seq=4
		auto packetDropper = make_shared<PacketDropper>(dropSeqs);
		auto nackGenerator = make_shared<TestNackGenerator>();
		auto receivingSession = make_shared<RtcpReceivingSession>();
		auto rtxInterceptor = make_shared<RtxWireInterceptor>(RTX_SSRC, RTX_PT);
		nackGenerator->addToChain(packetDropper);
		rtxInterceptor->addToChain(nackGenerator);
		receivingSession->addToChain(rtxInterceptor);
		t->setMediaHandler(receivingSession);

		std::atomic_store(&capturedRtxInterceptor, rtxInterceptor);
		std::atomic_store(&t2, t);
	});

	auto t1 = pc1.addTrack(media);

	auto rtpConfig =
	    make_shared<RtpPacketizationConfig>(PRIMARY_SSRC, CNAME, PRIMARY_PT, CLOCK_RATE);

	auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
	auto nackResponder = make_shared<RtcpNackResponder>();
	srReporter->addToChain(nackResponder);

	t1->setMediaHandler(srReporter);

	pc1.setLocalDescription();

	// Wait for connection and tracks to be open
	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	if (!at2 || !at2->isOpen() || !t1->isOpen())
		return TestResult(false, "Track is not open");

	// Build and send 5 RTP packets (seq 1-5) from sender; seq 2 and 4 will be dropped
	vector<byte> payload = {byte{0xDE}, byte{0xAD}, byte{0xBE}, byte{0xEF}};

	for (uint16_t seq = 1; seq <= 5; seq++) {
		vector<byte> rtpRaw(sizeof(RtpHeader) + payload.size());
		auto *rtp = reinterpret_cast<RtpHeader *>(rtpRaw.data());
		rtp->setPayloadType(PRIMARY_PT);
		rtp->setSeqNumber(seq);
		rtp->setTimestamp(seq * 3000);
		rtp->setSsrc(PRIMARY_SSRC);
		rtp->preparePacket();
		memcpy(rtpRaw.data() + sizeof(RtpHeader), payload.data(), payload.size());

		if (!t1->send(rtpRaw.data(), rtpRaw.size()))
			throw runtime_error("Couldn't send RTP packet seq=" + to_string(seq));
	}

	// Wait for both recovered packets (seq=2 and seq=4, dropped then recovered via RTX)
	auto futureSeq2 = recvPromiseSeq2.get_future();
	auto futureSeq4 = recvPromiseSeq4.get_future();

	if (futureSeq2.wait_for(10s) == future_status::timeout)
		throw runtime_error("Didn't receive recovered seq=2 packet on pc2");
	if (futureSeq4.wait_for(10s) == future_status::timeout)
		throw runtime_error("Didn't receive recovered seq=4 packet on pc2");

	// Verify each recovered packet
	struct Expected { uint16_t seq; rtc::binary data; };
	Expected expectations[] = {
		{2, futureSeq2.get()},
		{4, futureSeq4.get()},
	};

	for (auto &[expectedSeq, recovered] : expectations) {
		auto *rtp = reinterpret_cast<const RtpHeader *>(recovered.data());

		if (rtp->ssrc() != PRIMARY_SSRC)
			return TestResult(false, "Recovered packet seq=" + to_string(expectedSeq) + " has wrong SSRC");
		if (rtp->payloadType() != PRIMARY_PT)
			return TestResult(false, "Recovered packet seq=" + to_string(expectedSeq) + " has wrong payload type");
		if (rtp->seqNumber() != expectedSeq)
			return TestResult(false, "Recovered packet seq=" + to_string(expectedSeq) + " has wrong sequence number");
		if (rtp->timestamp() != expectedSeq * 3000)
			return TestResult(false, "Recovered packet seq=" + to_string(expectedSeq) + " has wrong timestamp");

		size_t headerSize = rtp->getSize();
		size_t pktPayloadSize = recovered.size() - headerSize;
		if (pktPayloadSize != payload.size())
			return TestResult(false, "Recovered payload size mismatch for seq=" + to_string(expectedSeq));

		const byte *pktPayload = reinterpret_cast<const byte *>(recovered.data()) + headerSize;
		if (memcmp(pktPayload, payload.data(), payload.size()) != 0)
			return TestResult(false, "Recovered payload content mismatch for seq=" + to_string(expectedSeq));

		cout << "RTX recovered seq=" << expectedSeq << ": SSRC=" << rtp->ssrc()
		     << " PT=" << (int)rtp->payloadType() << " timestamp=" << rtp->timestamp() << endl;
	}

	// Verify that retransmissions were actually sent on the RTX SSRC/PT on the wire.
	auto interceptor = std::atomic_load(&capturedRtxInterceptor);
	if (!interceptor || interceptor->mRtxPacketsSeen.load() == 0)
		return TestResult(false, "No RTX packets seen on the wire with RTX_SSRC=" +
		                             to_string(RTX_SSRC) + " RTX_PT=" + to_string(RTX_PT));

	cout << "RTX wire packets seen: " << interceptor->mRtxPacketsSeen.load() << endl;

	// Clean up
	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	if (!t1->isClosed() || !t2->isClosed())
		return TestResult(false, "Track is not closed");

	return TestResult(true);
}

// Unit test: Description::addRtx() auto-assigns RTX codecs to all media entries.
TestResult test_rtx_description_addrtx() {
	InitLogger(LogLevel::Debug);

	// Build a description with one video and one audio media, no RTX yet
	Description desc("v=0\r\n"
	                  "o=- 0 0 IN IP4 0.0.0.0\r\n"
	                  "s=-\r\n"
	                  "t=0 0\r\n",
	                  Description::Type::Offer);

	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(96);
	video.addVP8Codec(97);
	desc.addMedia(video);

	Description::Audio audio("audio", Description::Direction::SendOnly);
	audio.addOpusCodec(111);
	desc.addMedia(audio);

	// Precondition: no RTX codecs present
	{
		auto m0 = desc.media(0);
		auto *vid = std::get_if<Description::Media *>(&m0);
		if (!vid)
			return TestResult(false, "Expected media at index 0");
		if ((*vid)->isRtxEnabled())
			return TestResult(false, "Video should not have RTX before addRtx()");

		auto m1 = desc.media(1);
		auto *aud = std::get_if<Description::Media *>(&m1);
		if (!aud)
			return TestResult(false, "Expected media at index 1");
		if ((*aud)->isRtxEnabled())
			return TestResult(false, "Audio should not have RTX before addRtx()");
	}

	// Call the description-level addRtx with audio=true to cover both media types
	desc.addRtx(nullopt, true);

	// Verify video media: both H264 (96) and VP8 (97) should each have an RTX codec
	{
		auto m0 = desc.media(0);
		auto *vid = std::get_if<Description::Media *>(&m0);
		if (!vid)
			return TestResult(false, "Expected media at index 0 after addRtx");
		if (!(*vid)->isRtxEnabled())
			return TestResult(false, "Video should have RTX enabled after addRtx()");

		auto rtxForH264 = (*vid)->getRtxPayloadType(96);
		if (!rtxForH264.has_value())
			return TestResult(false, "H264 (PT 96) should have an RTX payload type");
		cout << "H264 RTX PT: " << rtxForH264.value() << endl;

		auto rtxForVP8 = (*vid)->getRtxPayloadType(97);
		if (!rtxForVP8.has_value())
			return TestResult(false, "VP8 (PT 97) should have an RTX payload type");
		cout << "VP8 RTX PT: " << rtxForVP8.value() << endl;

		// RTX payload types must differ from each other and from the primaries
		if (rtxForH264.value() == rtxForVP8.value())
			return TestResult(false, "RTX PTs for H264 and VP8 must be different");
		if (rtxForH264.value() == 96 || rtxForH264.value() == 97)
			return TestResult(false, "RTX PT for H264 must not collide with primary PTs");
		if (rtxForVP8.value() == 96 || rtxForVP8.value() == 97)
			return TestResult(false, "RTX PT for VP8 must not collide with primary PTs");

		// Verify RTX PTs are in the dynamic range [96, 127]
		if (rtxForH264.value() < 96 || rtxForH264.value() > 127)
			return TestResult(false, "RTX PT for H264 out of dynamic range");
		if (rtxForVP8.value() < 96 || rtxForVP8.value() > 127)
			return TestResult(false, "RTX PT for VP8 out of dynamic range");
	}

	// Verify audio media: Opus (111) should also have an RTX codec
	{
		auto m1 = desc.media(1);
		auto *aud = std::get_if<Description::Media *>(&m1);
		if (!aud)
			return TestResult(false, "Expected media at index 1 after addRtx");
		if (!(*aud)->isRtxEnabled())
			return TestResult(false, "Audio should have RTX enabled after addRtx()");

		auto rtxForOpus = (*aud)->getRtxPayloadType(111);
		if (!rtxForOpus.has_value())
			return TestResult(false, "Opus (PT 111) should have an RTX payload type");
		cout << "Opus RTX PT: " << rtxForOpus.value() << endl;

		if (rtxForOpus.value() < 96 || rtxForOpus.value() > 127)
			return TestResult(false, "RTX PT for Opus out of dynamic range");
		if (rtxForOpus.value() == 111)
			return TestResult(false, "RTX PT for Opus must not collide with primary PT");
	}

	cout << "Calling addRtx again should be idempotent (no duplicate RTX entries)" << endl;
	desc.addRtx(nullopt, true);
	{
		auto m0 = desc.media(0);
		auto *vid = std::get_if<Description::Media *>(&m0);
		auto pts = (*vid)->payloadTypes();
		// Should have: 96 (H264), 97 (VP8), rtx1, rtx2 = 4 total
		if (pts.size() != 4)
			return TestResult(false,
			                  "Video should have exactly 4 payload types after double addRtx(), got " +
			                      to_string(pts.size()));
	}

	cout << "Verify SDP round-trip: serialize and re-parse, check RTX survives" << endl;
	string sdp1 = desc.generateSdp();
	Description desc2(sdp1, Description::Type::Offer);
	{
		auto m0 = desc2.media(0);
		auto *vid = std::get_if<Description::Media *>(&m0);
		if (!vid)
			return TestResult(false, "Re-parsed description missing video media");
		if (!(*vid)->isRtxEnabled())
			return TestResult(false, "RTX not preserved after SDP round-trip for video");
		if (!(*vid)->getRtxPayloadType(96).has_value())
			return TestResult(false, "H264 RTX not preserved after SDP round-trip");
		if (!(*vid)->getRtxPayloadType(97).has_value())
			return TestResult(false, "VP8 RTX not preserved after SDP round-trip");
	}

	cout << "Description::addRtx() test passed" << endl;
	return TestResult(true);
}

// Unit test: Description::addRtx() with default audio=false skips audio media,
// and a subsequent call with audio=true then adds RTX to audio as well.
TestResult test_rtx_description_addrtx_no_audio() {
	InitLogger(LogLevel::Debug);

	Description desc("v=0\r\n"
	                  "o=- 0 0 IN IP4 0.0.0.0\r\n"
	                  "s=-\r\n"
	                  "t=0 0\r\n",
	                  Description::Type::Offer);

	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(96);
	desc.addMedia(video);

	Description::Audio audio("audio", Description::Direction::SendOnly);
	audio.addOpusCodec(111);
	desc.addMedia(audio);

	// Call addRtx() with the default (audio=false)
	desc.addRtx();

	// Video should have RTX
	auto m0 = desc.media(0);
	auto *vid = std::get_if<Description::Media *>(&m0);
	if (!vid || !(*vid)->isRtxEnabled())
		return TestResult(false, "Video should have RTX after addRtx()");

	// Audio should be untouched
	auto m1 = desc.media(1);
	auto *aud = std::get_if<Description::Media *>(&m1);
	if (!aud)
		return TestResult(false, "Expected audio media at index 1");
	if ((*aud)->isRtxEnabled())
		return TestResult(false, "Audio should NOT have RTX after addRtx() with audio=false");
	if ((*aud)->payloadTypes().size() != 1)
		return TestResult(false, "Audio payload types should be unchanged");

	// Repeated call with audio=false should still leave audio alone
	desc.addRtx();
	m1 = desc.media(1);
	aud = std::get_if<Description::Media *>(&m1);
	if ((*aud)->isRtxEnabled())
		return TestResult(false, "Audio should still not have RTX after second addRtx(audio=false)");

	// Now call with audio=true — audio should gain RTX while video stays idempotent
	desc.addRtx(nullopt, true);

	m0 = desc.media(0);
	vid = std::get_if<Description::Media *>(&m0);
	if ((*vid)->payloadTypes().size() != 2)
		return TestResult(false,
		                  "Video should still have 2 payload types (H264+RTX) after audio=true call, got " +
		                      to_string((*vid)->payloadTypes().size()));

	m1 = desc.media(1);
	aud = std::get_if<Description::Media *>(&m1);
	if (!(*aud)->isRtxEnabled())
		return TestResult(false, "Audio should have RTX after addRtx(nullopt, true)");
	if (!(*aud)->getRtxPayloadType(111).has_value())
		return TestResult(false, "Opus should have an RTX payload type after audio=true");

	// SDP round-trip: audio RTX should be preserved now that it exists
	string sdp1 = desc.generateSdp();
	Description desc2(sdp1, Description::Type::Offer);
	auto m1rt = desc2.media(1);
	auto *audRt = std::get_if<Description::Media *>(&m1rt);
	if (!audRt || !(*audRt)->isRtxEnabled())
		return TestResult(false, "Audio RTX not preserved after SDP round-trip");

	cout << "Description::addRtx() audio=false test passed" << endl;
	return TestResult(true);
}

// Negotiation test: offerer disables RTX when the answer lacks RTX support.
TestResult test_rtx_attribute() {
	InitLogger(LogLevel::Debug);

	// RTX negotiation via PeerConnection — offerer has RTX, answerer's
	// answer lacks RTX, so offerer's track should have RTX disabled after negotiation
	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = PORT_RANGE_BEGIN;
	config2.portRangeEnd = PORT_RANGE_END;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		pc2.setRemoteDescription(string(sdp));
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		pc2.addRemoteCandidate(string(candidate));
	});

	// Intercept pc2's answer: strip RTX before forwarding to pc1
	pc2.onLocalDescription([&pc1](Description sdp) {
		for (int i = 0; i < sdp.mediaCount(); ++i) {
			auto media = sdp.media(i);
			if (auto *m = std::get_if<Description::Media *>(&media))
				(*m)->disableRtx();
		}
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		pc1.addRemoteCandidate(string(candidate));
	});

	// Offerer adds track with RTX
	Description::Video media("video", Description::Direction::SendOnly);
	media.addH264Codec(PRIMARY_PT);
	media.addRtxCodec(RTX_PT, PRIMARY_PT, CLOCK_RATE);
	media.addSSRC(PRIMARY_SSRC, CNAME);
	media.addRtxSSRC(PRIMARY_SSRC, RTX_SSRC);

	auto t1 = pc1.addTrack(media);

	if (!t1->description().isRtxEnabled())
		return TestResult(false, "Offerer track should have RTX enabled before negotiation");

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

	// The offerer should have RTX disabled after receiving the answer without RTX
	if (t1->description().isRtxEnabled())
		return TestResult(false,
		                  "Offerer track should have RTX disabled after answer lacks RTX");

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	cout << "RTX negotiation test passed" << endl;
	return TestResult(true);
}

// End-to-end test: sender has two codecs (H264 + VP8) each with their own RTX PT.
// Packets from both codecs are dropped, and the test verifies that the retransmitted
// packets are wrapped/unwrapped using the correct per-codec RTX payload type.
TestResult test_rtx_multi_codec() {
	InitLogger(LogLevel::Debug);
	cout << "Multi-Codec RTX Test" << endl;

	static const uint8_t H264_PT = 96;
	static const uint8_t VP8_PT = 97;
	static const uint8_t H264_RTX_PT = 98;
	static const uint8_t VP8_RTX_PT = 99;
	static const SSRC MC_PRIMARY_SSRC = 1234;
	static const SSRC MC_RTX_SSRC = 5678;

	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = PORT_RANGE_BEGIN;
	config2.portRangeEnd = PORT_RANGE_END;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		cout << "Description 1: " << sdp << endl;
		pc2.setRemoteDescription(string(sdp));
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		pc2.addRemoteCandidate(string(candidate));
	});

	pc1.onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		pc1.addRemoteCandidate(string(candidate));
	});

	pc2.onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });

	// Set up sender track with two codecs and their RTX mappings
	Description::Video media("video", Description::Direction::SendOnly);
	media.addH264Codec(H264_PT);
	media.addRtxCodec(H264_RTX_PT, H264_PT, CLOCK_RATE);
	media.addVP8Codec(VP8_PT);
	media.addRtxCodec(VP8_RTX_PT, VP8_PT, CLOCK_RATE);
	media.setBitrate(BITRATE);
	media.addSSRC(MC_PRIMARY_SSRC, CNAME);
	media.addRtxSSRC(MC_PRIMARY_SSRC, MC_RTX_SSRC);

	shared_ptr<Track> t2;

	// We'll send 6 packets: seq 1-3 with H264_PT, seq 4-6 with VP8_PT
	// Drop seq=2 (H264) and seq=5 (VP8)
	promise<rtc::binary> recvPromiseSeq2;
	promise<rtc::binary> recvPromiseSeq5;
	const std::unordered_set<uint16_t> dropSeqs = {2, 5};

	pc2.onTrack([&](shared_ptr<Track> t) {
		string mid = t->mid();
		cout << "Track 2: Received track with mid \"" << mid << "\"" << endl;

		t->onOpen([mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is open" << endl; });

		t->onMessage(
		    [&recvPromiseSeq2, &recvPromiseSeq5](rtc::binary message) {
			    if (IsRtcp(message))
				    return;
			    if (message.size() < sizeof(RtpHeader))
				    return;
			    auto *rtp = reinterpret_cast<const RtpHeader *>(message.data());
			    uint16_t seq = rtp->seqNumber();
			    if (seq == 2)
				    recvPromiseSeq2.set_value(std::move(message));
			    else if (seq == 5)
				    recvPromiseSeq5.set_value(std::move(message));
		    },
		    nullptr);

		auto desc = t->description();
		desc.addSSRC(MC_PRIMARY_SSRC, CNAME);
		desc.addRtxSSRC(MC_PRIMARY_SSRC, MC_RTX_SSRC);
		t->setDescription(desc);

		auto packetDropper = make_shared<PacketDropper>(dropSeqs);
		auto nackGenerator = make_shared<TestNackGenerator>();
		auto receivingSession = make_shared<RtcpReceivingSession>();
		nackGenerator->addToChain(packetDropper);
		receivingSession->addToChain(nackGenerator);
		t->setMediaHandler(receivingSession);

		std::atomic_store(&t2, t);
	});

	auto t1 = pc1.addTrack(media);

	auto rtpConfig =
	    make_shared<RtpPacketizationConfig>(MC_PRIMARY_SSRC, CNAME, H264_PT, CLOCK_RATE);

	auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
	auto nackResponder = make_shared<RtcpNackResponder>();
	srReporter->addToChain(nackResponder);

	t1->setMediaHandler(srReporter);

	pc1.setLocalDescription();

	// Wait for connection and tracks to be open
	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	if (!at2 || !at2->isOpen() || !t1->isOpen())
		return TestResult(false, "Track is not open");

	// Build and send 6 RTP packets: seq 1-3 with H264_PT, seq 4-6 with VP8_PT
	vector<byte> payload = {byte{0xDE}, byte{0xAD}, byte{0xBE}, byte{0xEF}};

	for (uint16_t seq = 1; seq <= 6; seq++) {
		uint8_t pt = (seq <= 3) ? H264_PT : VP8_PT;
		vector<byte> rtpRaw(sizeof(RtpHeader) + payload.size());
		auto *rtp = reinterpret_cast<RtpHeader *>(rtpRaw.data());
		rtp->setPayloadType(pt);
		rtp->setSeqNumber(seq);
		rtp->setTimestamp(seq * 3000);
		rtp->setSsrc(MC_PRIMARY_SSRC);
		rtp->preparePacket();
		memcpy(rtpRaw.data() + sizeof(RtpHeader), payload.data(), payload.size());

		if (!t1->send(rtpRaw.data(), rtpRaw.size()))
			throw runtime_error("Couldn't send RTP packet seq=" + to_string(seq));
	}

	// Wait for both recovered packets
	auto futureSeq2 = recvPromiseSeq2.get_future();
	auto futureSeq5 = recvPromiseSeq5.get_future();

	if (futureSeq2.wait_for(10s) == future_status::timeout)
		throw runtime_error("Didn't receive recovered seq=2 (H264) packet");
	if (futureSeq5.wait_for(10s) == future_status::timeout)
		throw runtime_error("Didn't receive recovered seq=5 (VP8) packet");

	// Verify recovered H264 packet (seq=2)
	{
		auto recovered = futureSeq2.get();
		auto *rtp = reinterpret_cast<const RtpHeader *>(recovered.data());

		if (rtp->ssrc() != MC_PRIMARY_SSRC)
			return TestResult(false, "Recovered H264 packet has wrong SSRC");
		if (rtp->payloadType() != H264_PT)
			return TestResult(false, "Recovered H264 packet has wrong PT: expected " +
			                             to_string(H264_PT) + " got " +
			                             to_string(rtp->payloadType()));
		if (rtp->seqNumber() != 2)
			return TestResult(false, "Recovered H264 packet has wrong seq number");

		cout << "RTX recovered H264 seq=2: PT=" << (int)rtp->payloadType() << endl;
	}

	// Verify recovered VP8 packet (seq=5)
	{
		auto recovered = futureSeq5.get();
		auto *rtp = reinterpret_cast<const RtpHeader *>(recovered.data());

		if (rtp->ssrc() != MC_PRIMARY_SSRC)
			return TestResult(false, "Recovered VP8 packet has wrong SSRC");
		if (rtp->payloadType() != VP8_PT)
			return TestResult(false, "Recovered VP8 packet has wrong PT: expected " +
			                             to_string(VP8_PT) + " got " +
			                             to_string(rtp->payloadType()));
		if (rtp->seqNumber() != 5)
			return TestResult(false, "Recovered VP8 packet has wrong seq number");

		cout << "RTX recovered VP8 seq=5: PT=" << (int)rtp->payloadType() << endl;
	}

	// Clean up
	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	if (!t1->isClosed() || !t2->isClosed())
		return TestResult(false, "Track is not closed");

	cout << "Multi-codec RTX test passed" << endl;
	return TestResult(true);
}
