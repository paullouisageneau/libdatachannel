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
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

// Helper handler that scans every incoming compound RTCP message for BYE (PT=203) packets
// and reports the SSRCs found via a callback.
namespace {
class RtcpByeWatcher : public MediaHandler {
public:
	using Callback = std::function<void(std::vector<SSRC>)>;
	explicit RtcpByeWatcher(Callback cb) : mCallback(std::move(cb)) {}

	void incoming(message_vector &messages, const message_callback &send) override {
		for (const auto &msg : messages) {
			if (!msg || msg->type != Message::Control)
				continue;
			scan(*msg);
		}
		MediaHandler::incoming(messages, send);
	}

private:
	void scan(const binary &data) {
		size_t offset = 0;
		while (offset + sizeof(RtcpHeader) <= data.size()) {
			auto header = reinterpret_cast<const RtcpHeader *>(data.data() + offset);
			size_t length = header->lengthInBytes();
			if (offset + length > data.size())
				return;
			if (header->payloadType() == 203) {
				auto bye = reinterpret_cast<const RtcpBye *>(header);
				std::vector<SSRC> ssrcs;
				for (uint8_t i = 0; i < bye->ssrcCount(); ++i)
					ssrcs.push_back(bye->getSsrc(i));
				if (mCallback)
					mCallback(std::move(ssrcs));
			}
			offset += length;
		}
	}

	Callback mCallback;
};
} // namespace

// Unit test: round-trip RtcpBye struct construction and parsing
TestResult test_rtcp_bye_packet() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP BYE packet test" << endl;

	const SSRC ssrc1 = 0xCAFEBABE;
	const SSRC ssrc2 = 0xDEADBEEF;
	const SSRC ssrc3 = 0x12345678;

	size_t packetSize = RtcpBye::SizeWithSsrcs(3);
	auto message = make_message(packetSize, Message::Control);
	auto *bye = reinterpret_cast<RtcpBye *>(message->data());
	bye->preparePacket(3);
	bye->setSsrc(0, ssrc1);
	bye->setSsrc(1, ssrc2);
	bye->setSsrc(2, ssrc3);

	if (bye->header.payloadType() != 203)
		return TestResult(false, "BYE header has wrong payload type: " +
		                             to_string(bye->header.payloadType()));
	if (bye->header.version() != 2)
		return TestResult(false, "BYE header has wrong version");
	if (bye->ssrcCount() != 3)
		return TestResult(false, "BYE has wrong ssrc count: " + to_string(bye->ssrcCount()));
	if (bye->getSsrc(0) != ssrc1 || bye->getSsrc(1) != ssrc2 || bye->getSsrc(2) != ssrc3)
		return TestResult(false, "BYE SSRC values mismatch");
	if (bye->getSize() != packetSize)
		return TestResult(false, "BYE getSize mismatch: " + to_string(bye->getSize()) +
		                             " expected " + to_string(packetSize));

	cout << "RTCP BYE packet test passed" << endl;
	return TestResult(true);
}

// Unit test: a compound RR + SDES + BYE message (the shape Track::close emits)
// is parseable end-to-end.
TestResult test_rtcp_bye_compound_packet() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP BYE compound packet test" << endl;

	const SSRC ssrc = 0xABCD1234;
	const string cname = "test-cname";

	size_t rrSize = RtcpRr::SizeWithReportBlocks(0);
	size_t sdesSize = RtcpSdes::Size({{uint8_t(cname.size())}});
	size_t byeSize = RtcpBye::SizeWithSsrcs(1);
	size_t totalSize = rrSize + sdesSize + byeSize;

	auto message = make_message(totalSize, Message::Control);

	auto *rr = reinterpret_cast<RtcpRr *>(message->data());
	rr->preparePacket(ssrc, 0);

	auto *sdes = reinterpret_cast<RtcpSdes *>(message->data() + rrSize);
	auto *chunk = sdes->getChunk(0);
	chunk->setSSRC(ssrc);
	auto *item = chunk->getItem(0);
	item->type = 1;
	item->setText(cname);
	sdes->preparePacket(1);

	auto *bye = reinterpret_cast<RtcpBye *>(message->data() + rrSize + sdesSize);
	bye->preparePacket(1);
	bye->setSsrc(0, ssrc);

	// Walk the compound and verify all three packets parse correctly.
	size_t offset = 0;
	int sawRr = 0, sawSdes = 0, sawBye = 0;
	SSRC byeSsrc = 0;
	while (offset + sizeof(RtcpHeader) <= totalSize) {
		auto *hdr = reinterpret_cast<const RtcpHeader *>(message->data() + offset);
		size_t length = hdr->lengthInBytes();
		if (offset + length > totalSize)
			return TestResult(false, "Compound packet truncated at offset " + to_string(offset));
		switch (hdr->payloadType()) {
		case 201:
			sawRr++;
			break;
		case 202:
			sawSdes++;
			break;
		case 203: {
			sawBye++;
			auto *b = reinterpret_cast<const RtcpBye *>(hdr);
			if (b->ssrcCount() != 1)
				return TestResult(false, "BYE in compound has wrong ssrc count");
			byeSsrc = b->getSsrc(0);
			break;
		}
		default:
			return TestResult(false, "Unexpected packet type in compound: " +
			                             to_string(hdr->payloadType()));
		}
		offset += length;
	}

	if (sawRr != 1 || sawSdes != 1 || sawBye != 1)
		return TestResult(false, "Compound parse counts wrong: rr=" + to_string(sawRr) +
		                             " sdes=" + to_string(sawSdes) + " bye=" + to_string(sawBye));
	if (byeSsrc != ssrc)
		return TestResult(false, "BYE SSRC in compound mismatch");
	if (offset != totalSize)
		return TestResult(false, "Compound parse did not consume entire message");

	cout << "RTCP BYE compound packet test passed" << endl;
	return TestResult(true);
}

namespace {

// Set up two PeerConnections wired via local signalling, with pc1 sending a video track
// to pc2. Returns when both tracks are open, or returns false if anything went wrong.
// `byeWatcher` is chained on pc2's track to observe inbound BYE.
struct ByeIntegrationFixture {
	std::unique_ptr<PeerConnection> pc1;
	std::unique_ptr<PeerConnection> pc2;
	shared_ptr<Track> t1;
	shared_ptr<Track> t2;
};

bool setupByeFixture(ByeIntegrationFixture &fx, bool senderEnablesBye, SSRC ssrc,
                     shared_ptr<RtcpByeWatcher> watcher) {
	static const uint8_t PRIMARY_PT = 96;
	static const char *CNAME = "rtcp-bye-test";

	Configuration config1;
	config1.sendRtcpByeOnTrackClose = senderEnablesBye;
	fx.pc1 = std::make_unique<PeerConnection>(config1);

	Configuration config2;
	fx.pc2 = std::make_unique<PeerConnection>(config2);

	fx.pc1->onLocalDescription(
	    [&](Description sdp) { fx.pc2->setRemoteDescription(string(sdp)); });
	fx.pc1->onLocalCandidate(
	    [&](Candidate cand) { fx.pc2->addRemoteCandidate(string(cand)); });
	fx.pc2->onLocalDescription(
	    [&](Description sdp) { fx.pc1->setRemoteDescription(string(sdp)); });
	fx.pc2->onLocalCandidate(
	    [&](Candidate cand) { fx.pc1->addRemoteCandidate(string(cand)); });

	fx.pc2->onTrack([&fx, watcher, ssrc](shared_ptr<Track> t) {
		auto desc = t->description();
		desc.addSSRC(ssrc, CNAME);
		t->setDescription(desc);

		auto recvSession = make_shared<RtcpReceivingSession>();
		t->setMediaHandler(recvSession);
		t->chainMediaHandler(watcher);

		std::atomic_store(&fx.t2, t);
	});

	Description::Video media("video", Description::Direction::SendOnly);
	media.addH264Codec(PRIMARY_PT);
	media.addSSRC(ssrc, CNAME);
	fx.t1 = fx.pc1->addTrack(media);

	fx.pc1->setLocalDescription();

	int attempts = 10;
	shared_ptr<Track> t2;
	while ((!(t2 = std::atomic_load(&fx.t2)) || !t2->isOpen() || !fx.t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (fx.pc1->state() != PeerConnection::State::Connected ||
	    fx.pc2->state() != PeerConnection::State::Connected)
		return false;
	if (!t2 || !t2->isOpen() || !fx.t1->isOpen())
		return false;

	return true;
}

} // namespace

// Integration test: with sendRtcpByeOnTrackClose=true on the sender,
// closing pc1's track delivers a BYE for the track's SSRC to pc2.
TestResult test_track_close_sends_rtcp_bye() {
	InitLogger(LogLevel::Debug);
	cout << "Track close sends RTCP BYE test" << endl;

	const SSRC ssrc = 0xBEEFCAFE;

	promise<std::vector<SSRC>> byePromise;
	atomic<bool> byeReceived{false};
	auto watcher = make_shared<RtcpByeWatcher>(
	    [&byePromise, &byeReceived](std::vector<SSRC> ssrcs) {
		    bool expected = false;
		    if (byeReceived.compare_exchange_strong(expected, true))
			    byePromise.set_value(std::move(ssrcs));
	    });

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, /*senderEnablesBye=*/true, ssrc, watcher))
		return TestResult(false, "Failed to set up integration fixture");

	auto byeFuture = byePromise.get_future();

	// Closing the sender track should emit the BYE compound now (transport is still alive).
	fx.t1->close();

	if (byeFuture.wait_for(5s) != future_status::ready) {
		fx.pc1->close();
		fx.pc2->close();
		return TestResult(false, "Did not observe RTCP BYE within timeout");
	}

	auto ssrcs = byeFuture.get();
	if (ssrcs.size() != 1 || ssrcs[0] != ssrc) {
		string got = "[";
		for (size_t i = 0; i < ssrcs.size(); ++i) {
			if (i > 0)
				got += ",";
			got += to_string(ssrcs[i]);
		}
		got += "]";
		fx.pc1->close();
		fx.pc2->close();
		return TestResult(false, "BYE carried unexpected SSRC list: " + got);
	}

	fx.pc1->close();
	this_thread::sleep_for(500ms);
	fx.pc2->close();
	this_thread::sleep_for(500ms);

	cout << "Track close sends RTCP BYE test passed" << endl;
	return TestResult(true);
}

// Integration test: with the default config (sendRtcpByeOnTrackClose=false),
// closing pc1's track does NOT emit a BYE.
TestResult test_track_close_no_bye_when_disabled() {
	InitLogger(LogLevel::Debug);
	cout << "Track close no BYE when disabled test" << endl;

	const SSRC ssrc = 0xFEEDFACE;

	atomic<int> byeCount{0};
	auto watcher = make_shared<RtcpByeWatcher>(
	    [&byeCount](std::vector<SSRC>) { byeCount.fetch_add(1); });

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, /*senderEnablesBye=*/false, ssrc, watcher))
		return TestResult(false, "Failed to set up integration fixture");

	fx.t1->close();

	// Wait long enough that a BYE would have been delivered if sent.
	this_thread::sleep_for(2s);

	if (byeCount.load() != 0) {
		fx.pc1->close();
		fx.pc2->close();
		return TestResult(false, "Unexpected BYE received with default config: count=" +
		                             to_string(byeCount.load()));
	}

	fx.pc1->close();
	this_thread::sleep_for(500ms);
	fx.pc2->close();
	this_thread::sleep_for(500ms);

	cout << "Track close no BYE when disabled test passed" << endl;
	return TestResult(true);
}
