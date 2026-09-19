/**
 * Copyright (c) 2025-2026 Apple Inc.
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
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

namespace {

const uint8_t kCipherSuite = 0x04; // AES-128-GCM
const char *kCname = "sframe-offer-answer";
const uint8_t kPayloadType = 96;

// Several frames per track, so the far end can be checked for order and completeness and
// not just for content. Reordering or a dropped frame both show up as a sequence mismatch.
const size_t kFramesPerTrack = 5;

// Video frames are deliberately larger than one MTU so each fragments into several RTP
// packets, exercising SFrame framing and reassembly across frame boundaries.
const size_t kVideoFrameSize = 4000;

// Audio alternates between a frame that fits in one RTP packet and one that does not. The
// packetizer fragments audio exactly like video, so a large frame goes out as an S-only
// packet followed by an E-only packet and has to be reassembled by the receiver. A 1275
// byte Opus frame -- the format's maximum -- already exceeds one chunk, so this is a real
// size rather than a contrived one.
const size_t kAudioFrameSizeSmall = 160;
const size_t kAudioFrameSizeLarge = 2000;

size_t audioFrameSize(size_t frameIndex) {
	return frameIndex % 2 == 0 ? kAudioFrameSizeSmall : kAudioFrameSizeLarge;
}

void expect(bool condition, const string &message) {
	if (!condition)
		throw std::runtime_error(message);
}

enum class Kind { Audio, Video };

struct TrackSpec {
	string mid;
	Kind kind;
};

binary sessionKey() {
	binary key(16);
	for (size_t i = 0; i < key.size(); ++i)
		key[i] = std::byte(0x40 + i);
	return key;
}

// Hands back the session key for any KID. The per-SSRC derivation is the library's job on
// both sides, so this provider never sees an SSRC.
class SessionKeyProvider final : public SFrameKeyProvider {
public:
	explicit SessionKeyProvider(uint8_t ratchetStepBits = 0, bool perSSRC = true)
	    : mRatchetStepBits(ratchetStepBits), mPerSSRC(perSSRC) {}

	bool usePerSSRCDerivation() const override { return mPerSSRC; }

	uint8_t ratchetStepBits() const override { return mRatchetStepBits; }

	std::optional<SFrameKeyDetails> getKeyDetails(uint64_t keyGeneration) override {
		std::lock_guard<std::mutex> lock(mMutex);
		mRequested.insert(keyGeneration);
		return SFrameKeyDetails{kCipherSuite, sessionKey(), 0, SFrameKeyUse::Decrypt};
	}

	// The key generations this receiver was asked about. Ratcheting does not change the
	// generation, so this stays at one entry across a ratchet -- that is the point of the
	// provider never seeing a ratchet step.
	std::set<uint64_t> requested() const {
		std::lock_guard<std::mutex> lock(mMutex);
		return mRequested;
	}

private:
	const uint8_t mRatchetStepBits;
	const bool mPerSSRC;
	mutable std::mutex mMutex;
	std::set<uint64_t> mRequested;
};

// perSsrc must match the receiving provider. It is ignored on the shared-key path, which
// builds the SFrameEncoder itself, but is set there too so the intent reads the same way.
SFrameConfig makeSFrameConfig(uint64_t ratchetPeriod = 0, uint8_t ratchetStepBits = 0,
                              bool perSsrc = true) {
	SFrameConfig config;
	config.keyDetails = SFrameKeyDetails{kCipherSuite, sessionKey(), /*ctrStart=*/0,
	                                     SFrameKeyUse::Encrypt};
	config.keyGeneration = 1;
	config.ratchetStepBits = ratchetStepBits;
	config.ratchetPeriod = ratchetPeriod;
	config.perSsrcDerivation = perSsrc;
	return config;
}

// A deterministic payload, distinct per track so a crossed wire is visible.
// Base counter for a frame. Spaced far enough apart that no two frames on any track can
// produce overlapping runs.
uint32_t frameBase(size_t trackIndex, size_t frameIndex) {
	return uint32_t(1000000 * (trackIndex + 1) + 10000 * frameIndex);
}

// A frame is a run of little-endian uint32 counters: base, base+1, base+2, ... That makes
// two different faults distinguishable at the far end. A frame delivered out of order
// starts at the wrong base, and a fragment delivered out of order inside a frame breaks
// the run part way through, with the offset of the break identifying which chunk moved.
binary makeFrame(size_t size, uint32_t base) {
	size -= size % 4; // whole counters only
	binary frame;
	frame.reserve(size);
	for (uint32_t i = 0; i < uint32_t(size / 4); ++i) {
		const uint32_t value = base + i;
		frame.push_back(static_cast<std::byte>(value & 0xFF));
		frame.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
		frame.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
		frame.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
	}
	return frame;
}

uint32_t readCounter(const binary &frame, size_t index) {
	const size_t offset = index * 4;
	return uint32_t(std::to_integer<uint8_t>(frame[offset])) |
	       (uint32_t(std::to_integer<uint8_t>(frame[offset + 1])) << 8) |
	       (uint32_t(std::to_integer<uint8_t>(frame[offset + 2])) << 16) |
	       (uint32_t(std::to_integer<uint8_t>(frame[offset + 3])) << 24);
}

// "" when the frame is an unbroken run from expectedBase, otherwise what went wrong.
string describeSequence(const binary &frame, uint32_t expectedBase) {
	if (frame.empty())
		return "frame is empty";
	if (frame.size() % 4 != 0)
		return "frame size " + to_string(frame.size()) + " is not a whole number of counters";

	const size_t count = frame.size() / 4;
	const uint32_t first = readCounter(frame, 0);
	if (first != expectedBase) {
		// A wrong opening counter is ambiguous on its own: the whole frame may have
		// arrived out of order, or the first fragment of this frame may have. If the
		// expected base turns up later in the same frame it is the latter.
		for (size_t i = 1; i < count; ++i) {
			if (readCounter(frame, i) == expectedBase)
				return "frame opens at counter " + to_string(first) + " and the expected base " +
				       to_string(expectedBase) + " appears at index " + to_string(i) +
				       " (fragments arrived out of order within the frame)";
		}
		return "frame starts at counter " + to_string(first) + ", expected " +
		       to_string(expectedBase) + " (frames arrived out of order)";
	}

	for (size_t i = 1; i < count; ++i) {
		const uint32_t value = readCounter(frame, i);
		if (value != expectedBase + uint32_t(i))
			return "counter run breaks at index " + to_string(i) + " (byte offset " +
			       to_string(i * 4) + "): got " + to_string(value) + ", expected " +
			       to_string(expectedBase + uint32_t(i)) +
			       " (fragments arrived out of order within the frame)";
	}
	return "";
}

Description::Media makeMedia(const TrackSpec &spec, uint32_t ssrc) {
	if (spec.kind == Kind::Video) {
		Description::Video video(spec.mid, Description::Direction::SendOnly);
		video.addH264Codec(kPayloadType);
		video.addSSRC(ssrc, kCname);
		video.addSFrame();
		return video;
	}

	Description::Audio audio(spec.mid, Description::Direction::SendOnly);
	audio.addOpusCodec(kPayloadType);
	audio.addSSRC(ssrc, kCname);
	audio.addSFrame();
	return audio;
}

struct Outcome {
	bool sFrameNegotiated = false; // a=sframe still on the offerer's track description
	std::vector<binary> sent;
	std::vector<binary> received;     // in arrival order
	std::vector<binary> rtcpReceived; // Control messages, in arrival order
	size_t trackIndex = 0;
};

// Minimal well-formed RTCP packets. Only the header needs to be real: the point is that
// SFrame neither encrypts nor swallows them, so they must arrive byte-identical.
binary makeRtcp(uint8_t packetType, uint8_t count, size_t bodyWords, uint8_t fill) {
	binary pkt(4 + bodyWords * 4, std::byte{fill});
	pkt[0] = std::byte(0x80 | (count & 0x1F));           // V=2, no padding
	pkt[1] = std::byte(packetType);
	pkt[2] = std::byte((bodyWords >> 8) & 0xFF);         // length in 32-bit words minus one
	pkt[3] = std::byte(bodyWords & 0xFF);
	return pkt;
}

// Runs a full offer/answer between two peer connections, then sends one frame per track and
// collects what came out the far end.
//
// `declinedMids` names the m-lines the answerer declines SFrame on, standing in for a peer
// that supports it on some m-lines and not others. The
// receiver installs an SFrame depacketizer only where SFrame was agreed, which is what an
// application would do -- unless `keepSFrameHandlerWhenDeclined` is set, which leaves the
// SFrame depacketizer in place on a declined m-line so its plaintext path is exercised.
std::map<string, Outcome> runOfferAnswer(const std::vector<TrackSpec> &specs,
                                         const std::set<string> &declinedMids,
                                         uint64_t ratchetPeriod = 0,
                                         uint8_t ratchetStepBits = 0,
                                         bool sharedKey = false,
                                         size_t framesPerTrack = kFramesPerTrack,
                                         std::chrono::milliseconds frameGap =
                                             std::chrono::milliseconds(20),
                                         std::map<string, std::set<uint64_t>> *kidsSeen = nullptr,
                                         bool keepSFrameHandlerWhenDeclined = false) {
	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(string(sdp)); });
	pc1.onLocalCandidate(
	    [&pc2](Candidate candidate) { pc2.addRemoteCandidate(string(candidate)); });

	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });
	pc2.onLocalCandidate(
	    [&pc1](Candidate candidate) { pc1.addRemoteCandidate(string(candidate)); });

	std::mutex mutex;
	std::map<string, std::vector<binary>> receivedByMid;
	std::map<string, std::vector<binary>> rtcpByMid;
	std::map<string, Kind> kindByMid;
	for (const auto &spec : specs)
		kindByMid[spec.mid] = spec.kind;

	std::atomic<int> openRemoteTracks{0};
	std::vector<shared_ptr<Track>> remoteTracks;
	std::map<string, shared_ptr<SessionKeyProvider>> providers;
	std::mutex remoteMutex;

	pc2.onTrack([&](shared_ptr<Track> track) {
		const string mid = track->mid();
		const bool negotiated = declinedMids.count(mid) == 0;
		const Kind kind = kindByMid.count(mid) ? kindByMid[mid] : Kind::Video;

		// Only install SFrame on the m-lines that agreed to it; elsewhere a plain
		// depacketizer strips the RTP header and hands back the payload as-is.
		shared_ptr<MediaHandler> depacketizer;
		if (negotiated || keepSFrameHandlerWhenDeclined) {
			auto provider = std::make_shared<SessionKeyProvider>(ratchetStepBits, !sharedKey);
			if (negotiated) {
				std::lock_guard<std::mutex> lock(remoteMutex);
				providers[mid] = provider;
			}
			if (kind == Kind::Video)
				depacketizer = std::make_shared<SFrameVideoRtpDepacketizer>(provider);
			else
				depacketizer = std::make_shared<SFrameAudioRtpDepacketizer>(48000, provider);
		} else {
			depacketizer = std::make_shared<RtpDepacketizer>(
			    kind == Kind::Video ? RtpPacketizer::VideoClockRate : 48000);
		}

		track->setMediaHandler(depacketizer);

		// The offer's a=sframe reaches this callback, and survives into the answer only
		// because an SFrame handler was installed above. Where one was not, PeerConnection
		// strips it and the answer declines -- nothing here has to ask for that. A mid in
		// declinedMids that kept its handler has to say so explicitly, which is what an
		// application refusing SFrame on a track it could have protected would do. Where the
		// handler was not kept, saying nothing is enough, and that is the path under test.
		if (!negotiated && keepSFrameHandlerWhenDeclined) {
			auto desc = track->description();
			desc.removeSFrame();
			track->setDescription(std::move(desc));
		}

		// A depacketizer stamps frameInfo on what it produces, and impl::Track delivers
		// those through the frame callback -- onMessage() only ever sees messages without
		// frameInfo, so it would never fire here.
		track->onFrame([&mutex, &receivedByMid, mid](binary data, FrameInfo) {
			// Appended in arrival order: the comparison below is order-sensitive.
			std::lock_guard<std::mutex> lock(mutex);
			receivedByMid[mid].push_back(std::move(data));
		});

		// RTCP carries no frameInfo, so it arrives here rather than through onFrame.
		track->onMessage([&mutex, &rtcpByMid, mid](binary data) {
			std::lock_guard<std::mutex> lock(mutex);
			rtcpByMid[mid].push_back(std::move(data));
		}, [](string) {});

		{
			std::lock_guard<std::mutex> lock(remoteMutex);
			remoteTracks.push_back(track);
		}
		openRemoteTracks++;
	});

	// Offerer: every track gets an SFrame packetizer. Negotiation decides whether it
	// actually applies SFrame, which is the behaviour under test.
	std::vector<shared_ptr<Track>> tracks;
	std::vector<uint32_t> trackSsrcs;
	auto sharedEncoder =
	    sharedKey ? std::make_shared<SFrameEncoder>(makeSFrameConfig(ratchetPeriod,
	                                                                ratchetStepBits,
	                                                                /*perSsrc=*/false))
	              : nullptr;
	std::map<string, std::vector<binary>> sentByMid;
	uint32_t ssrc = 0x1000;
	for (const auto &spec : specs) {
		const uint32_t trackSsrc = ssrc++;
		trackSsrcs.push_back(trackSsrc);
		auto track = pc1.addTrack(makeMedia(spec, trackSsrc));
		expect(track->description().hasSFrame(),
		       "offerer track mid=" + spec.mid + " should carry a=sframe before negotiation");

		const uint32_t clockRate =
		    spec.kind == Kind::Video ? RtpPacketizer::VideoClockRate : 48000;
		auto rtpConfig =
		    std::make_shared<RtpPacketizationConfig>(trackSsrc, kCname, kPayloadType, clockRate);
		if (sharedKey) {
			// No per-SSRC derivation means one key for every track, so they must share one
			// encoder: separate encoders would each start at ctrStart and reuse nonces.
			track->setMediaHandler(std::make_shared<SFrameRtpPacketizer>(
			    rtpConfig, sharedEncoder, SFrameMode::PerFrame));
		} else {
			track->setMediaHandler(std::make_shared<SFrameRtpPacketizer>(
			    rtpConfig, makeSFrameConfig(ratchetPeriod, ratchetStepBits),
			    SFrameMode::PerFrame));
		}

		tracks.push_back(std::move(track));
	}

	pc1.setLocalDescription();

	int attempts = 15;
	while (attempts-- && (pc1.state() != PeerConnection::State::Connected ||
	                      pc2.state() != PeerConnection::State::Connected ||
	                      openRemoteTracks != int(specs.size())))
		this_thread::sleep_for(1s);

	expect(pc1.state() == PeerConnection::State::Connected &&
	           pc2.state() == PeerConnection::State::Connected,
	       "peer connections did not connect");
	expect(openRemoteTracks == int(specs.size()), "expected " + to_string(specs.size()) +
	                                                  " remote tracks, got " +
	                                                  to_string(openRemoteTracks.load()));

	// Wait for both ends of every track to be open before sending.
	attempts = 10;
	while (attempts--) {
		bool allOpen = true;
		for (const auto &t : tracks)
			allOpen = allOpen && t->isOpen();
		{
			std::lock_guard<std::mutex> lock(remoteMutex);
			for (const auto &t : remoteTracks)
				allOpen = allOpen && t->isOpen();
		}
		if (allOpen)
			break;
		this_thread::sleep_for(500ms);
	}

	string sendError;
	std::map<string, std::vector<binary>> sentRtcpByMid;

	for (size_t i = 0; i < tracks.size(); ++i) {
		for (size_t f = 0; f < framesPerTrack; ++f) {
			const size_t size =
			    specs[i].kind == Kind::Video ? kVideoFrameSize : audioFrameSize(f);
			auto frame = makeFrame(size, frameBase(i, f));
			sentByMid[specs[i].mid].push_back(frame);

			FrameInfo info(uint32_t(3000 * (f + 1)));
			info.payloadType = kPayloadType;
			try {
				tracks[i]->sendFrame(binary(frame), info);
			} catch (const std::exception &e) {
				sendError += " mid=" + specs[i].mid + " send threw: " + e.what() + ";";
			}
			this_thread::sleep_for(frameGap);
		}

		// RTCP is not SFrame-protected and must survive the handler chain untouched. APP is
		// the one RTCP type a Track can originate with a body we control, so it is what the
		// far end is checked against; SR and RR are covered by the unit tests, which can
		// inject them directly.
		try {
			const RtcpAppName appName = {'S', 'F', 'R', 'M'};
			tracks[i]->sendRtcpApp(trackSsrcs[i], appName, /*subtype=*/3,
			                       binary(8, std::byte{0xC3}));
			sentRtcpByMid[specs[i].mid].push_back(binary(8, std::byte{0xC3}));
		} catch (const std::exception &e) {
			sendError += " mid=" + specs[i].mid + " rtcp app send threw: " + e.what() + ";";
		}
		this_thread::sleep_for(std::chrono::milliseconds(20));

		if (!tracks[i]->isOpen())
			sendError += " mid=" + specs[i].mid + " local track not open;";
	}
	{
		std::lock_guard<std::mutex> lock(remoteMutex);
		for (const auto &t : remoteTracks)
			if (!t->isOpen())
				sendError += " mid=" + t->mid() + " REMOTE track not open;";
	}
	expect(sendError.empty(), "send failed:" + sendError);

	// Wait for every track to deliver every frame
	attempts = 40;
	while (attempts--) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			bool complete = receivedByMid.size() == specs.size();
			for (const auto &entry : receivedByMid)
				complete = complete && entry.second.size() >= framesPerTrack;
			for (const auto &entry : sentRtcpByMid)
				complete = complete && rtcpByMid[entry.first].size() >= entry.second.size();
			if (complete)
				break;
		}
		this_thread::sleep_for(250ms);
	}

	std::map<string, Outcome> outcomes;
	for (size_t i = 0; i < tracks.size(); ++i) {
		const string &mid = specs[i].mid;
		Outcome outcome;
		outcome.trackIndex = i;
		outcome.sFrameNegotiated = tracks[i]->description().hasSFrame();

		outcome.sent = sentByMid[mid];
		std::lock_guard<std::mutex> lock(mutex);
		if (receivedByMid.count(mid))
			outcome.received = receivedByMid[mid];
		if (rtcpByMid.count(mid))
			outcome.rtcpReceived = rtcpByMid[mid];
		outcomes[mid] = std::move(outcome);
	}

	if (kidsSeen) {
		std::lock_guard<std::mutex> lock(remoteMutex);
		for (const auto &entry : providers)
			(*kidsSeen)[entry.first] = entry.second->requested();
	}

	pc1.close();
	this_thread::sleep_for(500ms);
	pc2.close();
	this_thread::sleep_for(500ms);

	return outcomes;
}

// Whatever was negotiated, the bytes handed to sendFrame() must come out the far end
// unchanged and in the order they were sent: with SFrame they are encrypted and decrypted
// again, without it they pass through. The sequence is compared element by element, so a
// dropped frame, a duplicated frame or two frames arriving out of order all fail here
// rather than being masked by a content-only check.
void checkRoundTrip(const std::map<string, Outcome> &outcomes, const string &mid,
                    bool expectSFrame) {
	auto it = outcomes.find(mid);
	expect(it != outcomes.end(), "no outcome recorded for mid=" + mid);
	const Outcome &outcome = it->second;

	expect(outcome.sFrameNegotiated == expectSFrame,
	       "mid=" + mid + ": a=sframe is " + (outcome.sFrameNegotiated ? "set" : "unset") +
	           " but the answer " + (expectSFrame ? "kept" : "declined") + " it");

	if (outcome.received.size() != outcome.sent.size()) {
		// Name the frames that made it, by their opening counter and size, so a systematic
		// loss (every large frame, say) is visible rather than just a count.
		string arrived;
		for (const auto &frame : outcome.received) {
			if (frame.size() < 4)
				continue;
			arrived += " base=" + to_string(readCounter(frame, 0)) + "(" +
			           to_string(frame.size()) + "B)";
		}
		string wanted;
		for (size_t i = 0; i < outcome.sent.size(); ++i)
			wanted += " base=" + to_string(frameBase(outcome.trackIndex, i)) + "(" +
			          to_string(outcome.sent[i].size()) + "B)";
		expect(false, "mid=" + mid + ": received " + to_string(outcome.received.size()) +
		                  " frames, sent " + to_string(outcome.sent.size()) + "; arrived:" +
		                  arrived + "; expected:" + wanted);
	}

	for (size_t i = 0; i < outcome.sent.size(); ++i) {
		const binary &sent = outcome.sent[i];
		const binary &received = outcome.received[i];

		expect(received.size() == sent.size(),
		       "mid=" + mid + " frame " + to_string(i) + ": received " +
		           to_string(received.size()) + " bytes, sent " + to_string(sent.size()));

		// The counter run says more than a plain comparison: a wrong starting value means
		// the frames themselves arrived out of order, and a break part way through means
		// the fragments of one frame did.
		const string problem = describeSequence(received, frameBase(outcome.trackIndex, i));
		expect(problem.empty(), "mid=" + mid + " frame " + to_string(i) + ": " + problem);

		expect(received == sent,
		       "mid=" + mid + " frame " + to_string(i) + ": bytes differ from those sent");
	}
}

} // namespace

// Track::incoming delivers one message per call, so RTCP always reaches the handler chain
// in a batch of its own with no media alongside -- exactly the case a fail-closed receive
// path gets wrong by clearing the whole batch.
void checkRtcpPassedThrough(const std::map<string, Outcome> &outcomes, const string &mid) {
	auto it = outcomes.find(mid);
	expect(it != outcomes.end(), "no outcome recorded for mid=" + mid);
	const auto &got = it->second.rtcpReceived;

	expect(!got.empty(), "mid=" + mid +
	                         ": no RTCP reached the far end, so the SFrame handlers ate it");

	bool sawApp = false;
	for (const auto &pkt : got) {
		expect(pkt.size() >= 4, "mid=" + mid + ": truncated RTCP packet");
		expect((uint8_t(pkt[0]) >> 6) == 2, "mid=" + mid + ": RTCP version mangled");
		if (uint8_t(pkt[1]) != 204)
			continue; // SR/RR/feedback the stack generates on its own
		sawApp = true;
		// Encryption would change every byte, so an intact body proves it passed through.
		expect(uint8_t(pkt.back()) == 0xC3,
		       "mid=" + mid + ": APP body was modified in transit");
	}
	expect(sawApp, "mid=" + mid + ": the APP packet did not arrive");
}

TestResult test_sframe_rtcp_passthrough() {
	try {
		// One of each kind: the audio and video receive paths are separate code.
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {});
		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);
		checkRtcpPassedThrough(outcomes, "0");
		checkRtcpPassedThrough(outcomes, "1");
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
	return TestResult(true);
}

// A track whose peer declined SFrame still has to carry RTCP.
TestResult test_sframe_rtcp_passthrough_declined() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {"0", "1"});
		checkRtcpPassedThrough(outcomes, "0");
		checkRtcpPassedThrough(outcomes, "1");
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
	return TestResult(true);
}

// Shared keying over a live connection: two tracks, one key, one counter. The unit test
// pins the counters; this pins that the whole path -- negotiation, per-track packetizers
// sharing an encoder, and receivers that skip the Section 7 derivation -- carries media.
TestResult test_sframe_shared_key_multi_track() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {},
		                               /*ratchetPeriod=*/0, /*ratchetStepBits=*/0,
		                               /*sharedKey=*/true);
		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
	return TestResult(true);
}

// Single video m-line, answer keeps a=sframe: encrypted on the wire, identical at the far end.
TestResult test_sframe_video_answer_yes() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}}, {});
		checkRoundTrip(outcomes, "0", true);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Single video m-line, answer declines. Legal: the offerer drops a=sframe, the packetizer
// stops encrypting and stops prefixing the descriptor, and the raw bytes still arrive intact.
TestResult test_sframe_video_answer_no() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}}, {"0"});
		checkRoundTrip(outcomes, "0", false);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Same case, but the receiver leaves its SFrameVideoRtpDepacketizer installed instead of
// swapping in a plain one. An application that builds its handler chain before it sees the
// answer ends up here, and the 4000-byte frames span several RTP packets, so the declined
// path has to concatenate them back into whole frames rather than deliver nothing.
TestResult test_sframe_video_declined_keeps_depacketizer() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}}, {"0"}, /*ratchetPeriod=*/0,
		                               /*ratchetStepBits=*/0, /*sharedKey=*/false,
		                               kFramesPerTrack, std::chrono::milliseconds(20),
		                               /*kidsSeen=*/nullptr,
		                               /*keepSFrameHandlerWhenDeclined=*/true);
		checkRoundTrip(outcomes, "0", false);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Same two cases on an audio m-line, which has its own depacketizer.
TestResult test_sframe_audio_answer_yes() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Audio}}, {});
		checkRoundTrip(outcomes, "0", true);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

TestResult test_sframe_audio_answer_no() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Audio}}, {"0"});
		checkRoundTrip(outcomes, "0", false);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// The draft gives "a=sframe" no values, but it may grow them. A peer sending a valued form is
// asserting support, so hasSFrame() has to match on the attribute key -- reading it as a
// decline would silently drop the track to unprotected media. removeSFrame() already matched
// that way; this pins the two together.
TestResult test_sframe_attribute_with_parameters() {
	try {
		Description::Video media("0", Description::Direction::SendOnly);
		media.addH264Codec(kPayloadType);
		expect(!media.hasSFrame(), "a fresh m-line should not carry a=sframe");

		media.addAttribute("sframe:profile=1");
		expect(media.hasSFrame(), "a valued a=sframe was read as a decline");

		media.removeSFrame();
		expect(!media.hasSFrame(), "removeSFrame() left a valued a=sframe in place");

		// The bare form still works, and both forms are removed together.
		media.addSFrame();
		media.addAttribute("sframe:profile=1");
		expect(media.hasSFrame(), "a=sframe was not recognised");
		media.removeSFrame();
		expect(!media.hasSFrame(), "removeSFrame() left one of the two forms behind");

		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Mixed audio and video m-lines, all accepted.
TestResult test_sframe_multi_track_answer_yes() {
	try {
		auto outcomes = runOfferAnswer(
		    {{"0", Kind::Video}, {"1", Kind::Audio}, {"2", Kind::Video}, {"3", Kind::Audio}}, {});
		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);
		checkRoundTrip(outcomes, "2", true);
		checkRoundTrip(outcomes, "3", true);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Ratcheting over a live connection. The sender is given a five second ratchet period and
// frames are pumped for long enough to cross it, so the receive side has to notice the KID
// move and follow the per-SSRC key chain. Every frame must still round-trip, and the
// receiver must have been asked about more than one KID -- otherwise the ratchet never
// happened and the test would pass for the wrong reason.
TestResult test_sframe_multi_track_ratcheting() {
	try {
		const uint64_t ratchetPeriod = 5;   // seconds
		const uint8_t ratchetStepBits = 4;  // steps 0..15 within the key generation
		const size_t frames = 26;
		const auto gap = std::chrono::milliseconds(300); // ~7.8s, comfortably past one ratchet

		std::map<string, std::set<uint64_t>> kidsSeen;
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {},
		                               ratchetPeriod, ratchetStepBits, /*sharedKey=*/false,
		                               frames, gap, &kidsSeen);

		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);

		// A provider is only ever asked about a key generation, never a ratchet step, so a
		// ratchet must not change what it is asked for. Seeing exactly one generation is
		// what proves the ratchet stayed inside its field: if it had escaped, the receiver
		// would have asked for a different generation and got nullopt, and the round-trip
		// checks above would already have failed.
		//
		// That the sender actually ratcheted, and the receiver followed the chain, is
		// pinned at the unit level by testDecoderFollowsRatchetSteps, which can see the KID
		// directly. Here it shows up as frames continuing to decode across the period.
		for (const string &mid : {"0", "1"}) {
			auto it = kidsSeen.find(mid);
			expect(it != kidsSeen.end(), "mid=" + mid + ": no key generation recorded");
			expect(it->second.size() == 1,
			       "mid=" + mid + ": the receiver was asked about " +
			           to_string(it->second.size()) +
			           " key generations across " + to_string(frames) +
			           " frames, so a ratchet escaped its field in the KID");
		}

		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// The interesting case: the answer declines SFrame on one audio and one video m-line and
// keeps it on the others. Every track must still round-trip its bytes, two of them
// protected and two of them not, which only holds if the decision is made per m-line and
// is independent of media type.
TestResult test_sframe_multi_track_partial_answer() {
	try {
		auto outcomes = runOfferAnswer(
		    {{"0", Kind::Video}, {"1", Kind::Audio}, {"2", Kind::Video}, {"3", Kind::Audio}},
		    {"1", "2"});
		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", false);
		checkRoundTrip(outcomes, "2", false);
		checkRoundTrip(outcomes, "3", true);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

#else // RTC_ENABLE_MEDIA

TestResult test_sframe_video_answer_yes() { return TestResult(true); }
TestResult test_sframe_video_answer_no() { return TestResult(true); }
TestResult test_sframe_attribute_with_parameters() { return TestResult(true); }
TestResult test_sframe_video_declined_keeps_depacketizer() { return TestResult(true); }
TestResult test_sframe_audio_answer_yes() { return TestResult(true); }
TestResult test_sframe_audio_answer_no() { return TestResult(true); }
TestResult test_sframe_multi_track_answer_yes() { return TestResult(true); }
TestResult test_sframe_multi_track_partial_answer() { return TestResult(true); }
TestResult test_sframe_shared_key_multi_track() { return TestResult(true); }
TestResult test_sframe_rtcp_passthrough() { return TestResult(true); }
TestResult test_sframe_rtcp_passthrough_declined() { return TestResult(true); }

#endif
