/**
 * Copyright (c) 2026 Kostya Vasilyev
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

// impl::XrManager is internal-only (not part of the public API): it is self-contained (no
// Track/PeerConnection/MediaHandler involved), so its own logic is tested directly here rather
// than through a full PeerConnection loopback.
#include "impl/xrmanager.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

using namespace rtc;
using namespace std;
using namespace std::chrono;

namespace {

// RtcpXrDlrrSubBlock::dlrr() is expressed in units of 1/65536 seconds (RFC 3611 Section 4.5).
constexpr uint32_t kDlrrUnitsPerSecond = 65536;

// Builds a single (non-compound) RTCP XR packet containing one RRTR block.
message_ptr makeRrtrMessage(SSRC reporterSsrc, uint64_t ntpTimestamp) {
	size_t size = RtcpXr::HeaderSize() + RtcpXrRrtrBlock::Size();
	auto message = make_message(size, Message::Control);

	auto rrtr = reinterpret_cast<RtcpXrRrtrBlock *>(message->data() + RtcpXr::HeaderSize());
	rrtr->preparePacket();
	rrtr->setNtpTimestamp(ntpTimestamp);

	auto xr = reinterpret_cast<RtcpXr *>(message->data());
	xr->preparePacket(reporterSsrc, uint16_t(size / 4 - 1));

	return message;
}

// Independently parses whatever impl::XrManager::send() produced, without reusing any of its
// own code, so the test cross-checks the wire format rather than the implementation.
struct ParsedDlrr {
	SSRC senderSsrc;
	SSRC reporterSsrc;
	uint32_t ntpMiddle32Bit;
	uint32_t reportDelay;
};

// Parses raw bytes (independent of any Message::type tag) looking for a DLRR block in a single,
// non-compound XR packet - which is exactly what impl::XrManager::send() produces.
vector<ParsedDlrr> parseDlrrBytes(const byte *data, size_t size) {
	vector<ParsedDlrr> result;
	if (size < RtcpXr::HeaderSize())
		return result;

	auto xr = reinterpret_cast<const RtcpXr *>(data);
	if (xr->header.payloadType() != 207)
		return result;

	size_t blockOffset = RtcpXr::HeaderSize();
	size_t blockEnd = size;
	while (blockOffset + sizeof(RtcpXrBlockHeader) <= blockEnd) {
		auto blockHeader = reinterpret_cast<const RtcpXrBlockHeader *>(data + blockOffset);
		size_t blockLength = blockHeader->lengthInBytes();
		if (blockOffset + blockLength > blockEnd)
			break;

		if (blockHeader->blockType() == 5) {
			auto dlrr = reinterpret_cast<const RtcpXrDlrrBlock *>(data + blockOffset);
			for (int i = 0; i < dlrr->getSubBlockCount(); ++i) {
				auto sub = dlrr->getSubBlock(i);
				result.push_back({xr->senderSSRC(), sub->ssrc(), sub->lrr(), sub->dlrr()});
			}
		}

		blockOffset += blockLength;
	}
	return result;
}

vector<ParsedDlrr> parseDlrr(const message_ptr &message) {
	if (!message || message->type != Message::Control)
		return {};
	return parseDlrrBytes(message->data(), message->size());
}

} // namespace

// Unit test: a single RRTR is captured and answered with a matching DLRR.
TestResult test_xrmanager_rrtr_dlrr_roundtrip() {
	cout << "XrManager RRTR/DLRR round-trip test" << endl;

	const SSRC reporterSsrc = 424242;
	const SSRC localSsrc = 13131313;
	const uint64_t ntp = 0xCD001234'567800ABULL;

	impl::XrManager manager;
	manager.incoming(makeRrtrMessage(reporterSsrc, ntp));

	vector<message_ptr> sent;
	manager.send(localSsrc, [&](message_ptr m) { sent.push_back(std::move(m)); });

	if (sent.size() != 1)
		return TestResult(false, "Expected 1 DLRR message, got " + to_string(sent.size()));

	auto parsed = parseDlrr(sent[0]);
	if (parsed.size() != 1)
		return TestResult(false, "Expected 1 DLRR sub-block, got " + to_string(parsed.size()));

	if (parsed[0].senderSsrc != localSsrc)
		return TestResult(false, "DLRR packet sender SSRC mismatch");
	if (parsed[0].reporterSsrc != reporterSsrc)
		return TestResult(false, "DLRR sub-block SSRC mismatch");

	uint32_t expectedNtpMiddle32Bit = uint32_t(ntp >> 16);
	if (parsed[0].ntpMiddle32Bit != expectedNtpMiddle32Bit)
		return TestResult(false, "DLRR NTP middle 32 bits mismatch");

	if (parsed[0].reportDelay == 0 || parsed[0].reportDelay >= 2 * kDlrrUnitsPerSecond)
		return TestResult(false, "DLRR report delay out of the expected (0, 2s) range: " +
		                             to_string(parsed[0].reportDelay));

	cout << "XrManager RRTR/DLRR round-trip test passed" << endl;
	return TestResult(true);
}

// Unit test: send() is gated to roughly once per second, even with data pending.
TestResult test_xrmanager_gating() {
	cout << "XrManager gating test" << endl;

	const SSRC reporterSsrc = 1;
	const SSRC localSsrc = 2;

	impl::XrManager manager;
	manager.incoming(makeRrtrMessage(reporterSsrc, 0x0000000200000000ULL));

	int sendCount = 0;
	manager.send(localSsrc, [&](message_ptr) { sendCount++; });
	if (sendCount != 1)
		return TestResult(false, "Expected first send() to flush immediately");

	// Nothing pending anymore: send() should not fire again.
	manager.send(localSsrc, [&](message_ptr) { sendCount++; });
	if (sendCount != 1)
		return TestResult(false, "send() fired with nothing pending");

	// New data, but well within the flush interval: should still be gated.
	manager.incoming(makeRrtrMessage(reporterSsrc, 0x0000000300000000ULL));
	manager.send(localSsrc, [&](message_ptr) { sendCount++; });
	if (sendCount != 1)
		return TestResult(false, "send() fired again before the flush interval elapsed");

	this_thread::sleep_for(1100ms);
	manager.send(localSsrc, [&](message_ptr) { sendCount++; });
	if (sendCount != 2)
		return TestResult(false, "send() did not flush again after the interval elapsed");

	cout << "XrManager gating test passed" << endl;
	return TestResult(true);
}

// Unit test: RRTRs from distinct reporters are batched into one DLRR packet.
TestResult test_xrmanager_multiple_reporters_batched() {
	cout << "XrManager multiple reporters batched test" << endl;

	impl::XrManager manager;
	set<SSRC> reporters = {1001, 1002, 1003};
	for (SSRC ssrc : reporters)
		manager.incoming(makeRrtrMessage(ssrc, 0x0000000400000000ULL));

	vector<message_ptr> sent;
	manager.send(9999, [&](message_ptr m) { sent.push_back(std::move(m)); });

	if (sent.size() != 1)
		return TestResult(false, "Expected 1 packet, got " + to_string(sent.size()));

	auto parsed = parseDlrr(sent[0]);
	if (parsed.size() != reporters.size())
		return TestResult(false, "Expected " + to_string(reporters.size()) +
		                             " sub-blocks, got " + to_string(parsed.size()));

	set<SSRC> seen;
	for (const auto &p : parsed)
		seen.insert(p.reporterSsrc);
	if (seen != reporters)
		return TestResult(false, "Sub-block SSRCs do not match the reporters that sent RRTRs");

	cout << "XrManager multiple reporters batched test passed" << endl;
	return TestResult(true);
}

// Unit test: more reporters than fit in one packet are split across multiple DLRR packets.
TestResult test_xrmanager_chunking() {
	cout << "XrManager chunking test" << endl;

	const int reporterCount = 30; // more than the 25 sub-blocks per packet cap
	impl::XrManager manager;
	set<SSRC> reporters;
	for (int i = 0; i < reporterCount; ++i) {
		SSRC ssrc = SSRC(2000 + i);
		reporters.insert(ssrc);
		manager.incoming(makeRrtrMessage(ssrc, 0x0000000500000000ULL));
	}

	vector<message_ptr> sent;
	manager.send(1, [&](message_ptr m) { sent.push_back(std::move(m)); });

	if (sent.size() != 2)
		return TestResult(false, "Expected 2 packets, got " + to_string(sent.size()));

	set<SSRC> seen;
	int totalSubBlocks = 0;
	for (const auto &m : sent) {
		auto parsed = parseDlrr(m);
		totalSubBlocks += int(parsed.size());
		for (const auto &p : parsed)
			seen.insert(p.reporterSsrc);
	}

	if (totalSubBlocks != reporterCount)
		return TestResult(false, "Expected " + to_string(reporterCount) +
		                             " total sub-blocks across packets, got " +
		                             to_string(totalSubBlocks));
	if (seen != reporters)
		return TestResult(false, "Chunked sub-block SSRCs do not match all reporters");

	cout << "XrManager chunking test passed" << endl;
	return TestResult(true);
}

// End-to-end test: pc2 sends a real RTCP XR/RRTR packet with an arbitrary SSRC (unrelated to any
// track) over a real PeerConnection, and pc1 (the publisher) replies with a matching DLRR once it
// has sent at least one RTP packet, exercising both wiring points:
// PeerConnection::dispatchMedia() (capture) and Track::transportSend() (send).
//
// Deliberately no MediaHandler is attached on either track: a handler being present on the
// RecvOnly track would stop Track::outgoing() from auto-tagging the outgoing RRTR as an RTCP
// Control message (that auto-detection only kicks in when there's no handler at all), and it
// isn't needed anyway - with no handler, Track::incoming() just delivers every message,
// RTP and RTCP alike, through the ordinary onMessage() channel.
TestResult test_xrmanager_integration() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP XR integration test" << endl;

	static const SSRC MEDIA_SSRC = 5150;
	static const SSRC REPORTER_SSRC = 918273; // arbitrary, unrelated to any track's SSRC
	static const uint64_t RRTR_NTP = 0xAB00CD34'12005678ULL; // non-trivial, exercises the >>16 truncation
	static const uint8_t PRIMARY_PT = 96;
	static const uint16_t PORT_RANGE_BEGIN = 5100;
	static const uint16_t PORT_RANGE_END = 6100;
	static const char *CNAME = "rtcp-xr-send";

	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = PORT_RANGE_BEGIN;
	config2.portRangeEnd = PORT_RANGE_END;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(string(sdp)); });
	pc1.onLocalCandidate([&pc2](Candidate cand) { pc2.addRemoteCandidate(string(cand)); });

	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });
	pc2.onLocalCandidate([&pc1](Candidate cand) { pc1.addRemoteCandidate(string(cand)); });

	promise<vector<ParsedDlrr>> dlrrPromise;
	atomic<bool> dlrrReceived{false};

	shared_ptr<Track> t2;
	pc2.onTrack([&](shared_ptr<Track> t) {
		auto desc = t->description();
		desc.addSSRC(MEDIA_SSRC, CNAME);
		t->setDescription(desc);

		t->onMessage([&](message_variant data) {
			if (!holds_alternative<binary>(data))
				return;
			const binary &bytes = std::get<binary>(data);
			auto parsed = parseDlrrBytes(bytes.data(), bytes.size());
			if (parsed.empty())
				return;
			bool expected = false;
			if (dlrrReceived.compare_exchange_strong(expected, true))
				dlrrPromise.set_value(std::move(parsed));
		});

		std::atomic_store(&t2, t);
	});

	Description::Video media("video", Description::Direction::SendOnly);
	media.addH264Codec(PRIMARY_PT);
	media.addSSRC(MEDIA_SSRC, CNAME);
	auto t1 = pc1.addTrack(media);

	pc1.setLocalDescription();

	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");
	if (!at2 || !at2->isOpen() || !t1->isOpen())
		return TestResult(false, "Track is not open");

	// Build a raw RTCP XR/RRTR packet with an arbitrary reporter SSRC unrelated to MEDIA_SSRC.
	size_t rrtrSize = RtcpXr::HeaderSize() + RtcpXrRrtrBlock::Size();
	vector<byte> rrtrPacket(rrtrSize);
	auto rrtr = reinterpret_cast<RtcpXrRrtrBlock *>(rrtrPacket.data() + RtcpXr::HeaderSize());
	rrtr->preparePacket();
	rrtr->setNtpTimestamp(RRTR_NTP);
	auto xr = reinterpret_cast<RtcpXr *>(rrtrPacket.data());
	xr->preparePacket(REPORTER_SSRC, uint16_t(rrtrSize / 4 - 1));

	if (!at2->send(rrtrPacket.data(), rrtrPacket.size()))
		return TestResult(false, "Failed to send RTCP XR/RRTR packet from pc2");

	// Build a minimal fake RTP packet (version 2, dynamic payload type) for t1 to send: the
	// content doesn't matter, only that Track::transportSend() runs so the XR flush hook fires.
	vector fakeRtp(12, byte{0});
	fakeRtp[0] = byte{0x80}; // version 2
	fakeRtp[1] = byte{PRIMARY_PT};

	// RTCP travels over lossy UDP with no retransmission, so keep sending RTP (to drive the flush
	// hook) and retry the RRTR until the DLRR reply is observed or we give up. Each fake RTP
	// packet needs its own increasing sequence number, otherwise SRTP's replay protection
	// rejects repeats of the same (seq=0) packet.
	auto future = dlrrPromise.get_future();
	int sendAttempts = 60;
	uint16_t seq = 0;
	while (sendAttempts-- > 0) {
		fakeRtp[2] = byte((seq >> 8) & 0xff);
		fakeRtp[3] = byte(seq & 0xff);
		seq++;
		if (!t1->send(fakeRtp.data(), fakeRtp.size()))
			return TestResult(false, "Track::send returned false for fake RTP");
		if (sendAttempts % 10 == 0)
			at2->send(rrtrPacket.data(), rrtrPacket.size());
		if (future.wait_for(200ms) == future_status::ready)
			break;
	}

	if (future.wait_for(0s) != future_status::ready)
		return TestResult(false, "Did not receive RTCP XR/DLRR reply on pc2 after retries");

	auto parsed = future.get();
	const ParsedDlrr *match = nullptr;
	for (const auto &p : parsed) {
		if (p.reporterSsrc == REPORTER_SSRC) {
			match = &p;
			break;
		}
	}
	if (!match)
		return TestResult(false, "DLRR reply did not reference the RRTR's reporter SSRC");

	uint32_t expectedNtpMiddle32Bit = uint32_t(RRTR_NTP >> 16);
	if (match->ntpMiddle32Bit != expectedNtpMiddle32Bit)
		return TestResult(false, "DLRR reply had the wrong NTP middle 32 bits");

	if (match->reportDelay == 0 || match->reportDelay >= 2 * kDlrrUnitsPerSecond)
		return TestResult(false, "DLRR reply report delay out of the expected (0, 2s) range: " +
		                             to_string(match->reportDelay));

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	cout << "RTCP XR integration test passed" << endl;
	return TestResult(true);
}
