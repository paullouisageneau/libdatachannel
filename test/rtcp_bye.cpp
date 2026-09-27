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
#include <sstream>
#include <thread>
#include <vector>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

namespace {

// Collect the source list of each BYE (PT=203) packet in a compound RTCP message, one entry per
// BYE packet, so a single BYE listing several SSRCs is distinguishable from several BYEs
std::vector<std::vector<SSRC>> collectByeGroups(const binary &data) {
	std::vector<std::vector<SSRC>> result;
	size_t offset = 0;
	while (offset + sizeof(RtcpHeader) <= data.size()) {
		auto header = reinterpret_cast<const RtcpHeader *>(data.data() + offset);
		size_t length = header->lengthInBytes();
		if (length == 0 || offset + length > data.size())
			break;
		if (header->payloadType() == 203) {
			auto bye = reinterpret_cast<const RtcpBye *>(header);
			std::vector<SSRC> group;
			for (uint8_t i = 0; i < bye->getSSRCCount(); ++i)
				group.push_back(bye->getSSRC(i));
			result.push_back(std::move(group));
		}
		offset += length;
	}
	return result;
}

// Sender SSRC of the report packet leading a compound RTCP message, 0 if it does not lead with one
SSRC leadingReportSenderSsrc(const binary &data) {
	if (data.size() < sizeof(RtcpHeader))
		return 0;
	auto header = reinterpret_cast<const RtcpHeader *>(data.data());
	if (header->payloadType() == 200 && data.size() >= RtcpSr::Size(0))
		return reinterpret_cast<const RtcpSr *>(data.data())->senderSSRC();
	if (header->payloadType() == 201 && data.size() >= RtcpRr::SizeWithReportBlocks(0))
		return reinterpret_cast<const RtcpRr *>(data.data())->senderSSRC();
	return 0;
}

// Every BYE SSRC in a compound RTCP message, flattened
std::vector<SSRC> collectByeSsrcs(const binary &data) {
	std::vector<SSRC> result;
	for (const auto &group : collectByeGroups(data))
		for (auto ssrc : group)
			result.push_back(ssrc);
	return result;
}

// Helper handler reporting the SSRCs of inbound BYE packets via a callback
class RtcpByeWatcher final : public MediaHandler {
public:
	using Callback = std::function<void(std::vector<SSRC>)>;
	explicit RtcpByeWatcher(Callback cb) : mCallback(std::move(cb)) {}

	// Reports the sender SSRC of the report packet leading each compound that carries a BYE
	void onLeadingReportSender(std::function<void(SSRC)> cb) { mSenderCallback = std::move(cb); }

	void incoming(message_vector &messages, const message_callback &send) override {
		for (const auto &msg : messages) {
			if (!msg || msg->type != Message::Control)
				continue;
			auto groups = collectByeGroups(*msg);
			if (!groups.empty() && mSenderCallback)
				mSenderCallback(leadingReportSenderSsrc(*msg));

			// One callback per BYE packet, preserving its source list
			for (auto &group : groups)
				if (!group.empty() && mCallback)
					mCallback(std::move(group));
		}
		MediaHandler::incoming(messages, send);
	}

private:
	Callback mCallback;
	std::function<void(SSRC)> mSenderCallback;
};

// Send callback collecting whatever a handler pushes out
struct Collector {
	std::vector<message_ptr> messages;

	message_callback callback() {
		return [this](message_ptr m) { messages.push_back(std::move(m)); };
	}

	std::vector<std::vector<SSRC>> byeGroups() const {
		std::vector<std::vector<SSRC>> result;
		for (const auto &m : messages)
			for (auto &group : collectByeGroups(*m))
				result.push_back(std::move(group));
		return result;
	}

	std::vector<SSRC> byeSsrcs() const {
		std::vector<SSRC> result;
		for (const auto &m : messages)
			for (auto ssrc : collectByeSsrcs(*m))
				result.push_back(ssrc);
		return result;
	}
};

string formatSsrcs(const std::vector<SSRC> &ssrcs) {
	string result = "[";
	for (size_t i = 0; i < ssrcs.size(); ++i) {
		if (i > 0)
			result += ",";
		result += to_string(ssrcs[i]);
	}
	return result + "]";
}

} // namespace

using Dir = Description::Direction;

// Unit test: round-trip RtcpBye struct construction and parsing
TestResult test_rtcp_bye_packet() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP BYE packet test" << endl;

	const SSRC ssrc1 = 0xCAFEBABE;
	const SSRC ssrc2 = 0xDEADBEEF;
	const SSRC ssrc3 = 0x12345678;

	size_t packetSize = RtcpBye::SizeWithSSRCs(3);
	auto message = make_message(packetSize, Message::Control);
	auto *bye = reinterpret_cast<RtcpBye *>(message->data());
	bye->preparePacket(3);
	bye->setSSRC(0, ssrc1);
	bye->setSSRC(1, ssrc2);
	bye->setSSRC(2, ssrc3);

	if (bye->header.version() != 2)
		return TestResult(false, "BYE has wrong version");
	if (bye->header.payloadType() != 203)
		return TestResult(false,
		                  "BYE has wrong payload type: " + to_string(bye->header.payloadType()));
	if (bye->getSize() != packetSize)
		return TestResult(false, "BYE has wrong size: " + to_string(bye->getSize()) +
		                             ", expected " + to_string(packetSize));
	if (bye->getSSRCCount() != 3)
		return TestResult(false, "BYE has wrong SSRC count: " + to_string(bye->getSSRCCount()));
	if (bye->getSSRC(0) != ssrc1 || bye->getSSRC(1) != ssrc2 || bye->getSSRC(2) != ssrc3)
		return TestResult(false, "BYE SSRC mismatch: " + formatSsrcs(collectByeSsrcs(*message)));
	if (bye->getSSRC(3) != 0)
		return TestResult(false, "Out of range BYE SSRC is not 0");

	// The packet must be walkable as part of a compound
	auto parsed = collectByeSsrcs(*message);
	if (parsed.size() != 3)
		return TestResult(false, "Compound walk found " + to_string(parsed.size()) + " SSRCs");

	// Golden wire bytes, independent of the accessors that produced them:
	// V=2 P=0 SC=1 | PT=203 | length=1 | SSRC big-endian
	auto single = make_message(RtcpBye::SizeWithSSRCs(1), Message::Control);
	auto *one = reinterpret_cast<RtcpBye *>(single->data());
	one->preparePacket(1);
	one->setSSRC(0, 0xCAFEBABE);
	const uint8_t expected[8] = {0x81, 0xCB, 0x00, 0x01, 0xCA, 0xFE, 0xBA, 0xBE};
	if (single->size() != sizeof(expected) ||
	    std::memcmp(single->data(), expected, sizeof(expected)) != 0) {
		string got;
		for (size_t i = 0; i < single->size(); ++i) {
			char buf[4];
			snprintf(buf, sizeof(buf), "%02X ", uint8_t(single->data()[i]));
			got += buf;
		}
		return TestResult(false, "BYE wire bytes mismatch, got: " + got);
	}

	// RFC 3550 section 6.6: source count is 5 bits, and zero sources is useless
	bool rejectedZero = false, rejectedTooMany = false;
	try { one->preparePacket(0); } catch (const std::invalid_argument &) { rejectedZero = true; }
	try { one->preparePacket(32); } catch (const std::invalid_argument &) { rejectedTooMany = true; }
	if (!rejectedZero || !rejectedTooMany)
		return TestResult(false, "preparePacket accepted an invalid source count");

	cout << "RTCP BYE packet test passed" << endl;
	return TestResult(true);
}

// Unit test: RtcpSrReporter sends a BYE for its own SSRC, and the RTX SSRC paired with it, in a
// compound packet beginning with a sender report. The track passes down whether anything was
// transmitted (RFC 3550 6.3.7) and this handler suppresses the BYE when it was not.
TestResult test_rtcp_bye_sr_reporter() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP BYE sender report reporter test" << endl;

	const SSRC ssrc = 0xBEEFCAFE;
	auto rtpConfig = make_shared<RtpPacketizationConfig>(ssrc, "bye-test", 96, 90000);

	auto sendRtp = [&](shared_ptr<RtcpSrReporter> reporter, Collector &collector) {
		auto rtpMessage = make_message(sizeof(RtpHeader) + 4, Message::Binary);
		auto *rtp = reinterpret_cast<RtpHeader *>(rtpMessage->data());
		rtp->preparePacket();
		rtp->setPayloadType(96);
		rtp->setSeqNumber(1);
		rtp->setTimestamp(3000);
		rtp->setSsrc(ssrc);
		message_vector messages{std::move(rtpMessage)};
		reporter->outgoing(messages, collector.callback());
	};

	// RFC 3550 6.3.7: nothing was transmitted on the media, so no BYE
	{
		auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
		Collector collector;
		sendRtp(reporter, collector);
		collector.messages.clear();
		reporter->close(collector.callback(), Dir::SendOnly, /*sentPacket=*/false);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "BYE sent although nothing was transmitted");
	}

	// An inactive media has no active source, so no BYE even though RTCP may have been sent on it
	{
		auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
		Collector collector;
		sendRtp(reporter, collector);
		collector.messages.clear();
		reporter->close(collector.callback(), Dir::Inactive, /*sentPacket=*/true);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "BYE sent on an inactive media");
	}

	// RFC 8866 6.7: a media with no direction attribute is sendrecv, so Unknown still sends a BYE
	{
		auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
		Collector collector;
		sendRtp(reporter, collector);
		collector.messages.clear();
		reporter->close(collector.callback(), Dir::Unknown, /*sentPacket=*/true);
		if (collector.byeSsrcs().size() != 1)
			return TestResult(false, "No BYE on a media with an unspecified direction");
	}

	// On a receive-only media it has no sending SSRC to send a BYE for
	{
		auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
		Collector collector;
		sendRtp(reporter, collector);
		collector.messages.clear();
		reporter->close(collector.callback(), Dir::RecvOnly, /*sentPacket=*/true);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "BYE sent on a receive-only media");
	}

	// An unset SSRC is nothing meaningful to send a BYE for
	{
		auto unsetConfig = make_shared<RtpPacketizationConfig>(0, "bye-test", 96, 90000);
		auto reporter = make_shared<RtcpSrReporter>(unsetConfig);
		Collector collector;
		reporter->close(collector.callback(), Dir::SendOnly, /*sentPacket=*/true);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "BYE sent with an unset SSRC");
	}

	// Having sent RTP, closing sends the BYE inside a compound led by a sender report
	{
		auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
		Collector collector;
		sendRtp(reporter, collector);
		collector.messages.clear();
		reporter->close(collector.callback(), Dir::SendOnly, /*sentPacket=*/true);

		auto ssrcs = collector.byeSsrcs();
		if (ssrcs.size() != 1 || ssrcs[0] != ssrc)
			return TestResult(false, "Unexpected BYE SSRCs on close: " + formatSsrcs(ssrcs));
		if (collector.messages.size() != 1)
			return TestResult(false, "Expected the BYE in a single compound message");
		auto first = reinterpret_cast<const RtcpHeader *>(collector.messages[0]->data());
		if (first->payloadType() != 200)
			return TestResult(false, "Compound does not begin with a sender report, PT=" +
			                             to_string(first->payloadType()));
	}

	// The RTX SSRC is listed alongside the primary, primary first
	{
		const SSRC rtxSsrc = 0x0BADBEEF;
		Description::Video media("video", Description::Direction::SendOnly);
		media.addH264Codec(96);
		media.addSSRC(ssrc, "bye-test");
		media.addRtxSSRC(ssrc, rtxSsrc, "bye-test");

		auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
		reporter->media(media);

		Collector collector;
		sendRtp(reporter, collector);
		collector.messages.clear();
		reporter->close(collector.callback(), Dir::SendOnly, /*sentPacket=*/true);

		// Both SSRCs must be in the same BYE packet, primary first
		auto groups = collector.byeGroups();
		if (groups.size() != 1)
			return TestResult(false, "Expected exactly one BYE packet, got " +
			                             to_string(groups.size()));
		if (groups[0].size() != 2 || groups[0][0] != ssrc || groups[0][1] != rtxSsrc)
			return TestResult(false, "Expected the primary then the RTX SSRC in one BYE, got " +
			                             formatSsrcs(groups[0]));
	}

	cout << "RTCP BYE sender report reporter test passed" << endl;
	return TestResult(true);
}

// Unit test: RtcpReceivingSession sends a BYE for the SSRC it stamps on its RTCP, in a compound
// beginning with a receiver report. The track passes down whether anything was transmitted
// (RFC 3550 6.3.7) and this handler suppresses the BYE when it was not.
TestResult test_rtcp_bye_receiving_session() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP BYE receiving session test" << endl;

	const SSRC ssrc = 0xFEEDFACE;

	auto feedRtp = [&](shared_ptr<RtcpReceivingSession> session, Collector &collector) {
		auto rtpMessage = make_message(sizeof(RtpHeader) + 4, Message::Binary);
		auto *rtp = reinterpret_cast<RtpHeader *>(rtpMessage->data());
		rtp->preparePacket();
		rtp->setPayloadType(96);
		rtp->setSeqNumber(1);
		rtp->setTimestamp(3000);
		rtp->setSsrc(ssrc);
		message_vector messages{std::move(rtpMessage)};
		session->incoming(messages, collector.callback());
	};

	// No inbound packet seen, so no SSRC is known
	{
		auto session = make_shared<RtcpReceivingSession>();
		Collector collector;
		session->close(collector.callback(), Dir::RecvOnly, /*sentPacket=*/true);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "BYE sent although no SSRC was ever learned");
	}

	// RFC 3550 6.3.7: nothing was transmitted on the media, so no BYE
	{
		auto session = make_shared<RtcpReceivingSession>();
		Collector collector;
		feedRtp(session, collector);
		collector.messages.clear();
		session->close(collector.callback(), Dir::RecvOnly, /*sentPacket=*/false);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "BYE sent although nothing was transmitted");
	}

	{
		auto session = make_shared<RtcpReceivingSession>();
		Collector collector;
		feedRtp(session, collector);

		// On a receive-only media it sends a BYE for the SSRC it learned from the remote, compounded
		collector.messages.clear();
		session->close(collector.callback(), Dir::RecvOnly, /*sentPacket=*/true);

		auto ssrcs = collector.byeSsrcs();
		if (ssrcs.size() != 1 || ssrcs[0] != ssrc)
			return TestResult(false, "Unexpected BYE SSRCs on close: " + formatSsrcs(ssrcs));
		if (collector.messages.size() != 1)
			return TestResult(false, "Expected the BYE in a single compound message");
		auto first = reinterpret_cast<const RtcpHeader *>(collector.messages[0]->data());
		if (first->payloadType() != 201)
			return TestResult(false, "Compound does not begin with a receiver report, PT=" +
			                             to_string(first->payloadType()));

		// On a media with a send direction it defers to the handler owning the local sending SSRC
		collector.messages.clear();
		session->close(collector.callback(), Dir::SendRecv, /*sentPacket=*/true);
		if (!collector.byeSsrcs().empty())
			return TestResult(false, "Receiving session did not defer on a sending media");
	}

	cout << "RTCP BYE receiving session test passed" << endl;
	return TestResult(true);
}

namespace {

// Two PeerConnections wired via local signalling, pc1 sending a video track to pc2. pc1's track
// carries an RtcpSrReporter unless suppressed, pc2's carries an RtcpReceivingSession, and a BYE
// watcher may be chained on either side.
struct ByeIntegrationFixture {
	std::unique_ptr<PeerConnection> pc1;
	std::unique_ptr<PeerConnection> pc2;
	shared_ptr<Track> t1;
	shared_ptr<Track> t2;
	shared_ptr<RtpPacketizationConfig> rtpConfig;

	~ByeIntegrationFixture() {
		if (pc1)
			pc1->close();
		if (pc2)
			pc2->close();
		this_thread::sleep_for(500ms);
	}
};

bool setupByeFixture(ByeIntegrationFixture &fx, SSRC ssrc, shared_ptr<RtcpByeWatcher> watcher2,
                     shared_ptr<RtcpByeWatcher> watcher1 = nullptr,
                     bool installSenderHandler = true, SSRC rtxSsrc = 0) {
	static const uint8_t PAYLOAD_TYPE = 96;
	static const char *CNAME = "rtcp-bye-test";

	fx.pc1 = std::make_unique<PeerConnection>(Configuration());
	fx.pc2 = std::make_unique<PeerConnection>(Configuration());

	fx.pc1->onLocalDescription([&](Description sdp) { fx.pc2->setRemoteDescription(string(sdp)); });
	fx.pc1->onLocalCandidate([&](Candidate cand) { fx.pc2->addRemoteCandidate(string(cand)); });
	fx.pc2->onLocalDescription([&](Description sdp) { fx.pc1->setRemoteDescription(string(sdp)); });
	fx.pc2->onLocalCandidate([&](Candidate cand) { fx.pc1->addRemoteCandidate(string(cand)); });

	// pc2 receives: a full RTCP session, so it answers SRs with RRs and therefore transmits
	fx.pc2->onTrack([&fx, watcher2](shared_ptr<Track> t) {
		t->setMediaHandler(make_shared<RtcpReceivingSession>());
		if (watcher2)
			t->chainMediaHandler(watcher2);
		std::atomic_store(&fx.t2, t);
	});

	Description::Video media("video", Description::Direction::SendOnly);
	media.addH264Codec(PAYLOAD_TYPE);
	if (rtxSsrc != 0)
		media.addRtxCodec(PAYLOAD_TYPE + 1, PAYLOAD_TYPE, 90000);
	media.addSSRC(ssrc, CNAME);
	if (rtxSsrc != 0)
		media.addRtxSSRC(ssrc, rtxSsrc, CNAME);
	fx.t1 = fx.pc1->addTrack(media);

	fx.rtpConfig = make_shared<RtpPacketizationConfig>(ssrc, CNAME, PAYLOAD_TYPE, 90000);
	if (installSenderHandler)
		fx.t1->setMediaHandler(make_shared<RtcpSrReporter>(fx.rtpConfig));
	if (watcher1)
		fx.t1->chainMediaHandler(watcher1);

	fx.pc1->setLocalDescription();

	int attempts = 10;
	shared_ptr<Track> t2;
	while ((!(t2 = std::atomic_load(&fx.t2)) || !t2->isOpen() || !fx.t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (fx.pc1->state() != PeerConnection::State::Connected ||
	    fx.pc2->state() != PeerConnection::State::Connected)
		return false;

	return t2 && t2->isOpen() && fx.t1->isOpen();
}

bool sendRtpPacket(const ByeIntegrationFixture &fx, uint16_t seqNumber) {
	binary raw(sizeof(RtpHeader) + 4);
	auto *rtp = reinterpret_cast<RtpHeader *>(raw.data());
	rtp->preparePacket();
	rtp->setPayloadType(fx.rtpConfig->payloadType);
	rtp->setSeqNumber(seqNumber);
	rtp->setTimestamp(3000 * seqNumber);
	rtp->setSsrc(fx.rtpConfig->ssrc);
	return fx.t1->send(std::move(raw));
}

} // namespace

namespace {

// Several tracks on one connection, so RTCP is demultiplexed by SSRC rather than taking the
// single-track shortcut in PeerConnection::dispatchMedia(). Every track on both sides carries both
// RTCP handlers, chained in alternating order, so which of them sends the BYE is decided by the
// media direction alone and not by chain position.
//
//   video0  pc1 sends, RTX negotiated  -> RtcpSrReporter sends BYE{primary, rtx}
//   video1  pc1 sends, no RTX          -> RtcpSrReporter sends BYE{primary}
//   video2  pc1 receives, pc2 sends    -> RtcpReceivingSession sends BYE{learned SSRC}
struct MultiTrackFixture {
	static constexpr int TRACK_COUNT = 3;
	static constexpr int RECV_INDEX = 2; // the one pc1 only receives on
	static constexpr uint8_t PAYLOAD_TYPE = 96;
	static constexpr uint8_t RTX_PAYLOAD_TYPE = 97;

	std::unique_ptr<PeerConnection> pc1;
	std::unique_ptr<PeerConnection> pc2;
	std::vector<shared_ptr<Track>> tracks;                       // on pc1, in mid order
	std::vector<shared_ptr<RtpPacketizationConfig>> rtpConfigs;   // on pc1, one per track

	std::mutex mutex;
	std::vector<std::vector<SSRC>> byes;          // source list of each BYE arriving at pc2
	std::vector<shared_ptr<Track>> remoteTracks;  // on pc2; must be retained or they are removed
	shared_ptr<Track> senderTrack;                // pc2's send side of video2
	shared_ptr<RtpPacketizationConfig> senderConfig;
	int openTracks = 0;

	~MultiTrackFixture() {
		if (pc1)
			pc1->close();
		if (pc2)
			pc2->close();
		this_thread::sleep_for(500ms);
	}

	static string midFor(int i) { return "video" + to_string(i); }
	static bool pc1Sends(int i) { return i != RECV_INDEX; }
	static bool hasRtx(int i) { return i == 0; }
	static SSRC ssrcFor(int i) { return 0xB0000010 + SSRC(i); }     // pc1's sending SSRCs
	static SSRC rtxSsrcFor(int i) { return 0xB0000020 + SSRC(i); }  // pc1's RTX SSRCs
	static SSRC remoteSsrcFor(int i) { return 0xB0000030 + SSRC(i); } // pc2's sending SSRCs

	// What closing pc1's track i must put on the wire
	static std::vector<SSRC> expectedBye(int i) {
		if (!pc1Sends(i))
			return {remoteSsrcFor(i)}; // sent by the receiving session, learned from pc2
		if (hasRtx(i))
			return {ssrcFor(i), rtxSsrcFor(i)};
		return {ssrcFor(i)};
	}

	std::vector<std::vector<SSRC>> seenByes() {
		std::lock_guard<std::mutex> lock(mutex);
		return byes;
	}

	bool waitForByes(size_t n) {
		for (int i = 0; i < 100; ++i) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (byes.size() >= n)
					return true;
			}
			this_thread::sleep_for(100ms);
		}
		return false;
	}

	static bool sendRtpOn(const shared_ptr<Track> &track,
	                      const shared_ptr<RtpPacketizationConfig> &config, uint16_t seqNumber) {
		binary raw(sizeof(RtpHeader) + 4);
		auto *rtp = reinterpret_cast<RtpHeader *>(raw.data());
		rtp->preparePacket();
		rtp->setPayloadType(config->payloadType);
		rtp->setSeqNumber(seqNumber);
		rtp->setTimestamp(3000u * seqNumber);
		rtp->setSsrc(config->ssrc);
		return track->send(std::move(raw));
	}

	// Get RTP flowing in both directions so every track has transmitted something
	bool primeAllTracks() {
		for (int i = 0; i < TRACK_COUNT; ++i)
			if (pc1Sends(i) && !sendRtpOn(tracks[i], rtpConfigs[i], 1))
				return false;

		shared_ptr<Track> sender;
		shared_ptr<RtpPacketizationConfig> config;
		{
			std::lock_guard<std::mutex> lock(mutex);
			sender = senderTrack;
			config = senderConfig;
		}
		if (!sender || !config || !sendRtpOn(sender, config, 1))
			return false;

		// Let the SR/RR exchange happen so the receiving sessions have transmitted too
		this_thread::sleep_for(1500ms);
		return true;
	}
};

// Both RTCP handlers, plus the BYE watcher, on every track of either side. reporterFirst varies
// which of the two heads the chain: the direction alone is supposed to decide which one sends the
// BYE, so both orders must behave identically.
void chainBothHandlers(const shared_ptr<Track> &track, SSRC ssrc, const string &cname,
                       shared_ptr<RtcpByeWatcher> watcher, bool reporterFirst,
                       shared_ptr<RtpPacketizationConfig> *outConfig = nullptr) {
	auto rtpConfig = make_shared<RtpPacketizationConfig>(ssrc, cname,
	                                                     MultiTrackFixture::PAYLOAD_TYPE, 90000);
	auto reporter = make_shared<RtcpSrReporter>(rtpConfig);
	auto session = make_shared<RtcpReceivingSession>();

	if (reporterFirst) {
		track->setMediaHandler(reporter);
		track->chainMediaHandler(session);
	} else {
		track->setMediaHandler(session);
		track->chainMediaHandler(reporter);
	}
	if (watcher)
		track->chainMediaHandler(watcher);
	if (outConfig)
		*outConfig = std::move(rtpConfig);
}

// Returns an empty string on success, otherwise the reason it could not be set up
string setupMultiTrackFixture(MultiTrackFixture &fx) {
	fx.pc1 = std::make_unique<PeerConnection>(Configuration());
	fx.pc2 = std::make_unique<PeerConnection>(Configuration());

	fx.pc1->onLocalDescription([&](Description sdp) { fx.pc2->setRemoteDescription(string(sdp)); });
	fx.pc1->onLocalCandidate([&](Candidate cand) { fx.pc2->addRemoteCandidate(string(cand)); });
	fx.pc2->onLocalDescription([&](Description sdp) { fx.pc1->setRemoteDescription(string(sdp)); });
	fx.pc2->onLocalCandidate([&](Candidate cand) { fx.pc1->addRemoteCandidate(string(cand)); });

	fx.pc2->onTrack([&fx](shared_ptr<Track> t) {
		// Recorded without deduplication: a compound naming several SSRCs of one media, such as a
		// BYE listing a primary and its RTX SSRC, must still be delivered to the track only once
		auto watcher = make_shared<RtcpByeWatcher>([&fx](std::vector<SSRC> ssrcs) {
			std::lock_guard<std::mutex> lock(fx.mutex);
			fx.byes.push_back(std::move(ssrcs));
		});

		// pc1's receive-only media reciprocates to a sending one here, so pc2 needs an SSRC of its
		// own on it and has to put RTP on the wire for pc1's receiving session to report on
		int index = -1;
		for (int i = 0; i < MultiTrackFixture::TRACK_COUNT; ++i)
			if (t->mid() == MultiTrackFixture::midFor(i))
				index = i;

		bool pc2Sends = index >= 0 && !MultiTrackFixture::pc1Sends(index);
		auto ssrc = pc2Sends ? MultiTrackFixture::remoteSsrcFor(index)
		                     : MultiTrackFixture::remoteSsrcFor(index) + 0x100;
		auto cname = "bye-multi-remote-" + to_string(index);

		if (pc2Sends) {
			auto desc = t->description();
			desc.addSSRC(ssrc, cname);
			t->setDescription(std::move(desc));
		}

		shared_ptr<RtpPacketizationConfig> config;
		chainBothHandlers(t, ssrc, cname, watcher, /*reporterFirst=*/index % 2 != 0, &config);

		std::lock_guard<std::mutex> lock(fx.mutex);
		if (pc2Sends) {
			fx.senderTrack = t;
			fx.senderConfig = std::move(config);
		}
		fx.remoteTracks.push_back(std::move(t));
		++fx.openTracks;
	});

	for (int i = 0; i < MultiTrackFixture::TRACK_COUNT; ++i) {
		auto cname = "bye-multi-" + to_string(i);
		auto direction = MultiTrackFixture::pc1Sends(i) ? Description::Direction::SendOnly
		                                               : Description::Direction::RecvOnly;
		Description::Video media(MultiTrackFixture::midFor(i), direction);
		media.addH264Codec(MultiTrackFixture::PAYLOAD_TYPE);
		if (MultiTrackFixture::hasRtx(i))
			media.addRtxCodec(MultiTrackFixture::RTX_PAYLOAD_TYPE, MultiTrackFixture::PAYLOAD_TYPE,
			                  90000);
		if (MultiTrackFixture::pc1Sends(i)) {
			media.addSSRC(MultiTrackFixture::ssrcFor(i), cname);
			if (MultiTrackFixture::hasRtx(i))
				media.addRtxSSRC(MultiTrackFixture::ssrcFor(i), MultiTrackFixture::rtxSsrcFor(i),
				                 cname);
		}

		auto track = fx.pc1->addTrack(media);
		shared_ptr<RtpPacketizationConfig> config;
		chainBothHandlers(track, MultiTrackFixture::ssrcFor(i), cname, nullptr,
		                  /*reporterFirst=*/i % 2 == 0, &config);

		fx.tracks.push_back(std::move(track));
		fx.rtpConfigs.push_back(std::move(config));
	}

	fx.pc1->setLocalDescription();

	for (int attempts = 0; attempts < 20; ++attempts) {
		bool allOpen = true;
		for (auto &t : fx.tracks)
			if (!t->isOpen())
				allOpen = false;
		int seen;
		{
			std::lock_guard<std::mutex> lock(fx.mutex);
			seen = fx.openTracks;
		}
		if (allOpen && seen == MultiTrackFixture::TRACK_COUNT)
			break;
		this_thread::sleep_for(500ms);
	}

	std::ostringstream reason;
	if (fx.pc1->state() != PeerConnection::State::Connected)
		reason << " pc1 not connected;";
	if (fx.pc2->state() != PeerConnection::State::Connected)
		reason << " pc2 not connected;";
	int notOpen = 0;
	for (auto &t : fx.tracks)
		if (!t->isOpen())
			++notOpen;
	if (notOpen)
		reason << " " << notOpen << " of " << MultiTrackFixture::TRACK_COUNT
		       << " pc1 tracks not open;";
	{
		std::lock_guard<std::mutex> lock(fx.mutex);
		if (fx.openTracks != MultiTrackFixture::TRACK_COUNT)
			reason << " onTrack fired " << fx.openTracks << " of "
			       << MultiTrackFixture::TRACK_COUNT << " times;";
		if (!fx.senderTrack)
			reason << " pc2 never got the sending track;";
	}
	if (MultiTrackFixture::hasRtx(0) && !fx.tracks[0]->description().getRtxSsrcForSsrc(
	                                        MultiTrackFixture::ssrcFor(0)))
		reason << " RTX association lost during negotiation;";

	auto text = reason.str();
	return text.empty() ? string() : "multi-track fixture:" + text;
}

string describe(const std::vector<std::vector<SSRC>> &groups) {
	string result;
	for (const auto &g : groups)
		result += formatSsrcs(g);
	return result.empty() ? "[]" : result;
}

} // namespace

// Integration test: with several tracks on one connection, closing them one at a time sends a BYE
// only for the track being closed and leaves the others alone. Every track carries both RTCP
// handlers, so this also pins which handler speaks for each direction.
TestResult test_multiple_tracks_close_one_by_one() {
	InitLogger(LogLevel::Debug);
	cout << "Multiple tracks closed one by one test" << endl;

	MultiTrackFixture fx;
	if (auto err = setupMultiTrackFixture(fx); !err.empty())
		return TestResult(false, err);
	if (!fx.primeAllTracks())
		return TestResult(false, "Failed to get RTP flowing on every track");

	for (int i = 0; i < MultiTrackFixture::TRACK_COUNT; ++i) {
		fx.tracks[i]->close();

		if (!fx.waitForByes(size_t(i) + 1))
			return TestResult(false, "No BYE after closing " + MultiTrackFixture::midFor(i) +
			                             ", saw " + describe(fx.seenByes()));

		// Only the tracks closed so far may have had a BYE sent, in that order, each carrying
		// exactly the SSRCs of its own media
		auto seen = fx.seenByes();
		if (seen.size() != size_t(i) + 1)
			return TestResult(false, "Expected " + to_string(i + 1) + " BYEs after closing " +
			                             MultiTrackFixture::midFor(i) + ", saw " + describe(seen));
		for (int j = 0; j <= i; ++j)
			if (seen[size_t(j)] != MultiTrackFixture::expectedBye(j))
				return TestResult(false, "BYE for " + MultiTrackFixture::midFor(j) + " carried " +
				                             formatSsrcs(seen[size_t(j)]) + ", expected " +
				                             formatSsrcs(MultiTrackFixture::expectedBye(j)));
	}

	cout << "Multiple tracks closed one by one test passed" << endl;
	return TestResult(true);
}

// Integration test: with several tracks on one connection, closing the PeerConnection sends a BYE
// for every track, each carrying that media's own SSRCs.
TestResult test_multiple_tracks_peerconnection_close() {
	InitLogger(LogLevel::Debug);
	cout << "Multiple tracks PeerConnection close test" << endl;

	MultiTrackFixture fx;
	if (auto err = setupMultiTrackFixture(fx); !err.empty())
		return TestResult(false, err);
	if (!fx.primeAllTracks())
		return TestResult(false, "Failed to get RTP flowing on every track");

	fx.pc1->close();

	if (!fx.waitForByes(MultiTrackFixture::TRACK_COUNT))
		return TestResult(false, "Did not see a BYE for every track, saw " +
		                             describe(fx.seenByes()));

	// Order is not guaranteed here, so match each expected source list once
	auto seen = fx.seenByes();
	for (int i = 0; i < MultiTrackFixture::TRACK_COUNT; ++i) {
		auto expected = MultiTrackFixture::expectedBye(i);
		bool found = false;
		for (auto &group : seen)
			if (group == expected)
				found = true;
		if (!found)
			return TestResult(false, "No BYE matching " + MultiTrackFixture::midFor(i) + " " +
			                             formatSsrcs(expected) + ", saw " + describe(seen));
	}

	cout << "Multiple tracks PeerConnection close test passed" << endl;
	return TestResult(true);
}

// Integration test: closing a track which has been sending media delivers a BYE for its SSRC
TestResult test_track_close_sends_rtcp_bye() {
	InitLogger(LogLevel::Debug);
	cout << "Track close sends RTCP BYE test" << endl;

	const SSRC ssrc = 0xBEEFCAFE;

	promise<std::vector<SSRC>> byePromise;
	atomic<bool> byeReceived{false};
	auto watcher = make_shared<RtcpByeWatcher>([&](std::vector<SSRC> ssrcs) {
		bool expected = false;
		if (byeReceived.compare_exchange_strong(expected, true))
			byePromise.set_value(std::move(ssrcs));
	});

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, watcher))
		return TestResult(false, "Failed to set up integration fixture");

	if (!sendRtpPacket(fx, 1))
		return TestResult(false, "Failed to send RTP packet");

	auto byeFuture = byePromise.get_future();

	// Closing the sender track must emit the BYE now, while the transport is still alive
	fx.t1->close();

	if (byeFuture.wait_for(5s) != future_status::ready)
		return TestResult(false, "Did not observe RTCP BYE within timeout");

	auto ssrcs = byeFuture.get();
	if (ssrcs.size() != 1 || ssrcs[0] != ssrc)
		return TestResult(false, "BYE carried unexpected SSRC list: " + formatSsrcs(ssrcs));

	cout << "Track close sends RTCP BYE test passed" << endl;
	return TestResult(true);
}

// Integration test: closing the whole PeerConnection also delivers a BYE, i.e. the track handlers
// are closed before the transports are torn down
TestResult test_peerconnection_close_sends_rtcp_bye() {
	InitLogger(LogLevel::Debug);
	cout << "PeerConnection close sends RTCP BYE test" << endl;

	const SSRC ssrc = 0x0BADF00D;

	promise<std::vector<SSRC>> byePromise;
	atomic<bool> byeReceived{false};
	auto watcher = make_shared<RtcpByeWatcher>([&](std::vector<SSRC> ssrcs) {
		bool expected = false;
		if (byeReceived.compare_exchange_strong(expected, true))
			byePromise.set_value(std::move(ssrcs));
	});

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, watcher))
		return TestResult(false, "Failed to set up integration fixture");

	if (!sendRtpPacket(fx, 1))
		return TestResult(false, "Failed to send RTP packet");

	auto byeFuture = byePromise.get_future();

	// Close the sending PeerConnection rather than the track
	fx.pc1->close();

	if (byeFuture.wait_for(5s) != future_status::ready)
		return TestResult(false, "Did not observe RTCP BYE within timeout");

	auto ssrcs = byeFuture.get();
	if (ssrcs.size() != 1 || ssrcs[0] != ssrc)
		return TestResult(false, "BYE carried unexpected SSRC list: " + formatSsrcs(ssrcs));

	cout << "PeerConnection close sends RTCP BYE test passed" << endl;
	return TestResult(true);
}

// Integration test: with RTX negotiated, the BYE delivered to the peer carries the primary and
// the RTX SSRC in a single packet.
TestResult test_track_close_bye_carries_rtx_ssrc() {
	InitLogger(LogLevel::Debug);
	cout << "Track close BYE carries RTX SSRC test" << endl;

	const SSRC ssrc = 0xA11CE000;
	const SSRC rtxSsrc = 0xA11CE001;

	promise<std::vector<SSRC>> byePromise;
	atomic<bool> byeReceived{false};
	auto watcher = make_shared<RtcpByeWatcher>([&](std::vector<SSRC> ssrcs) {
		bool expected = false;
		if (byeReceived.compare_exchange_strong(expected, true))
			byePromise.set_value(std::move(ssrcs));
	});

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, watcher, nullptr, /*installSenderHandler=*/true, rtxSsrc))
		return TestResult(false, "Failed to set up integration fixture");

	// RTX must have survived negotiation, otherwise this would silently test nothing
	if (!fx.t1->description().getRtxSsrcForSsrc(ssrc))
		return TestResult(false, "RTX SSRC association was lost during negotiation");

	if (!sendRtpPacket(fx, 1))
		return TestResult(false, "Failed to send RTP packet");

	auto byeFuture = byePromise.get_future();
	fx.t1->close();

	if (byeFuture.wait_for(5s) != future_status::ready)
		return TestResult(false, "Did not observe RTCP BYE within timeout");

	// The watcher reports one callback per BYE packet, so this list is a single packet's sources
	auto ssrcs = byeFuture.get();
	if (ssrcs.size() != 2 || ssrcs[0] != ssrc || ssrcs[1] != rtxSsrc)
		return TestResult(false, "Expected the primary then the RTX SSRC in one BYE, got " +
		                             formatSsrcs(ssrcs));

	cout << "Track close BYE carries RTX SSRC test passed" << endl;
	return TestResult(true);
}

// Integration test: a RecvOnly track which only ever transmitted RTCP (an RR answering the
// sender's SR) must still emit a BYE when it is closed
TestResult test_recvonly_track_close_sends_rtcp_bye() {
	InitLogger(LogLevel::Debug);
	cout << "RecvOnly track close sends RTCP BYE test" << endl;

	const SSRC ssrc = 0x0DDBA11;

	promise<std::vector<SSRC>> byePromise;
	atomic<bool> byeReceived{false};
	auto watcherOnSender = make_shared<RtcpByeWatcher>([&](std::vector<SSRC> ssrcs) {
		bool expected = false;
		if (byeReceived.compare_exchange_strong(expected, true))
			byePromise.set_value(std::move(ssrcs));
	});

	atomic<SSRC> leadingSender{0};
	watcherOnSender->onLeadingReportSender([&](SSRC s) { leadingSender.store(s); });

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, nullptr, watcherOnSender))
		return TestResult(false, "Failed to set up integration fixture");

	// Sending RTP makes pc1 emit an SR, which makes pc2's receiving session answer with an RR.
	// pc2 has therefore transmitted RTCP, without ever sending RTP.
	if (!sendRtpPacket(fx, 1))
		return TestResult(false, "Failed to send RTP packet");

	auto byeFuture = byePromise.get_future();

	// Give the SR/RR exchange time to happen before closing the receiving side
	this_thread::sleep_for(1s);

	auto t2 = std::atomic_load(&fx.t2);
	if (!t2)
		return TestResult(false, "No receiving track");
	t2->close();

	if (byeFuture.wait_for(5s) != future_status::ready)
		return TestResult(false, "Did not observe RTCP BYE from the RecvOnly track within timeout");

	// The receiving session stamps its RTCP with the SSRC it learned from the remote, so that is
	// what its BYE carries too
	auto ssrcs = byeFuture.get();
	if (ssrcs.size() != 1 || ssrcs[0] != ssrc)
		return TestResult(false, "BYE carried unexpected SSRC list: " + formatSsrcs(ssrcs));

	// The compound leads with a receiver report stamped with the same learned SSRC, which is the
	// field a peer would use for RFC 3550 section 8.2 collision detection. Pinned so that giving
	// the receiving session an SSRC of its own has to update this deliberately.
	if (leadingSender.load() != ssrc)
		return TestResult(false, "Leading report sender SSRC was " +
		                             to_string(leadingSender.load()) + ", expected " +
		                             to_string(ssrc));

	cout << "RecvOnly track close sends RTCP BYE test passed" << endl;
	return TestResult(true);
}

// Guards the close ordering: processRemoteDescription() invokes onTrack synchronously while it
// holds mTracksMutex exclusively, so nothing on the close path may take that mutex on the calling
// thread. closeDataChannels/closeTracks/closeTransports must stay processor tasks for that to
// hold. The work runs on a detached thread so a hang is reported instead of wedging the suite.
TestResult test_close_from_ontrack_does_not_deadlock() {
	InitLogger(LogLevel::Debug);
	cout << "Close from onTrack does not deadlock test" << endl;

	auto donePromise = make_shared<promise<bool>>();
	auto doneFuture = donePromise->get_future();
	auto onTrackFired = make_shared<atomic<bool>>(false);

	std::thread([donePromise, onTrackFired]() {
		bool ok = true;
		try {
			auto pc1 = std::make_unique<PeerConnection>(Configuration());
			auto pc2 = std::make_unique<PeerConnection>(Configuration());
			auto *raw1 = pc1.get();
			auto *raw2 = pc2.get();

			pc1->onLocalDescription(
			    [raw2](Description sdp) { raw2->setRemoteDescription(string(sdp)); });
			pc1->onLocalCandidate([raw2](Candidate c) { raw2->addRemoteCandidate(string(c)); });
			pc2->onLocalDescription(
			    [raw1](Description sdp) { raw1->setRemoteDescription(string(sdp)); });
			pc2->onLocalCandidate([raw1](Candidate c) { raw1->addRemoteCandidate(string(c)); });

			// The user rejects the incoming track by closing the PeerConnection outright
			pc2->onTrack([raw2, onTrackFired](shared_ptr<Track>) {
				onTrackFired->store(true);
				raw2->close();
			});

			Description::Video media("video", Description::Direction::SendOnly);
			media.addH264Codec(96);
			media.addSSRC(0x42424242, "deadlock-test");
			auto t1 = pc1->addTrack(media);
			pc1->setLocalDescription();

			this_thread::sleep_for(2s);
			pc1->close();
			this_thread::sleep_for(500ms);
		} catch (const std::exception &e) {
			cerr << "Exception in deadlock test thread: " << e.what() << endl;
			ok = false;
		}
		donePromise->set_value(ok);
	}).detach();

	if (doneFuture.wait_for(20s) != future_status::ready)
		return TestResult(false, "Deadlocked closing the PeerConnection from onTrack");
	if (!doneFuture.get())
		return TestResult(false, "Exception while closing the PeerConnection from onTrack");
	if (!onTrackFired->load())
		return TestResult(false, "onTrack never fired, so the close path was not exercised");

	cout << "Close from onTrack does not deadlock test passed" << endl;
	return TestResult(true);
}

// Integration test: closing the PeerConnection reaches the Closed state and stops delivering
// callbacks synchronously, even though the transports are detached on the processor afterwards so
// that closing tracks can still send their BYE.
TestResult test_peerconnection_close_is_synchronous() {
	InitLogger(LogLevel::Debug);
	cout << "PeerConnection close is synchronous test" << endl;

	const SSRC ssrc = 0x5111CE00;

	atomic<int> byeCount{0};
	auto watcher = make_shared<RtcpByeWatcher>([&](std::vector<SSRC>) { byeCount.fetch_add(1); });

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, watcher))
		return TestResult(false, "Failed to set up integration fixture");

	if (!sendRtpPacket(fx, 1))
		return TestResult(false, "Failed to send RTP packet");

	// Any state change delivered after close() returns would mean the callbacks outlived it
	atomic<int> statesAfterClose{0};
	atomic<bool> closeReturned{false};
	fx.pc1->onStateChange([&](PeerConnection::State) {
		if (closeReturned.load())
			statesAfterClose.fetch_add(1);
	});

	fx.pc1->close();
	closeReturned.store(true);

	// The state must already be Closed, not settle into it later on the processor
	if (fx.pc1->state() != PeerConnection::State::Closed)
		return TestResult(false, "close() returned without reaching the Closed state");

	// ...and the BYE must still have gone out, which is what the deferred detach buys
	for (int i = 0; i < 50 && byeCount.load() == 0; ++i)
		this_thread::sleep_for(100ms);
	if (byeCount.load() == 0)
		return TestResult(false, "No BYE was delivered when closing the PeerConnection");

	this_thread::sleep_for(1s);
	if (statesAfterClose.load() != 0)
		return TestResult(false, "State change callback fired after close() returned, count=" +
		                             to_string(statesAfterClose.load()));

	cout << "PeerConnection close is synchronous test passed" << endl;
	return TestResult(true);
}

// Integration test: the BYE is RTCP behaviour, produced by the RTCP handlers. A track with an
// empty handler chain sends raw RTP and emits no BYE on close, even though it did transmit.
TestResult test_track_close_no_bye_without_rtcp_handler() {
	InitLogger(LogLevel::Debug);
	cout << "Track close no BYE without RTCP handler test" << endl;

	const SSRC ssrc = 0xC0FFEE00;

	atomic<int> byeCount{0};
	auto watcher = make_shared<RtcpByeWatcher>([&](std::vector<SSRC>) { byeCount.fetch_add(1); });

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, watcher, nullptr, /*installSenderHandler=*/false))
		return TestResult(false, "Failed to set up integration fixture");

	// The track really does transmit, so the absence of a BYE is down to the empty chain
	if (!sendRtpPacket(fx, 1))
		return TestResult(false, "Failed to send RTP packet");

	fx.t1->close();

	// Wait long enough that a BYE would have been delivered if sent
	this_thread::sleep_for(2s);

	if (byeCount.load() != 0)
		return TestResult(false, "Unexpected BYE received with no RTCP handler, count=" +
		                             to_string(byeCount.load()));

	cout << "Track close no BYE without RTCP handler test passed" << endl;
	return TestResult(true);
}

// Integration test: a track which never sent anything must not emit a BYE on close
TestResult test_track_close_no_bye_when_idle() {
	InitLogger(LogLevel::Debug);
	cout << "Track close no BYE when idle test" << endl;

	const SSRC ssrc = 0xFEEDFACE;

	atomic<int> byeCount{0};
	auto watcher = make_shared<RtcpByeWatcher>([&](std::vector<SSRC>) { byeCount.fetch_add(1); });

	ByeIntegrationFixture fx;
	if (!setupByeFixture(fx, ssrc, watcher))
		return TestResult(false, "Failed to set up integration fixture");

	fx.t1->close();

	// Wait long enough that a BYE would have been delivered if sent
	this_thread::sleep_for(2s);

	if (byeCount.load() != 0)
		return TestResult(false, "Unexpected BYE received, count=" + to_string(byeCount.load()));

	cout << "Track close no BYE when idle test passed" << endl;
	return TestResult(true);
}
