/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframeutility.hpp"
#include "rtc/rtc.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
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

// Cipher suite per media kind. Real deployments pick a shorter tag for audio, where frames are
// small and frequent, and a longer one for video -- RFC 9605 Section 4.5 gives exactly that
// example. Three profiles are spread across the cases below, so both key sizes, both hash families
// and both AEAD constructions run over a live connection rather than only in the crypto test
// vectors.
struct SuiteProfile {
	uint16_t video;
	uint16_t audio;
};

const SuiteProfile kAes256Ctr{0x06, 0x08}; // AES-256-CTR+HMAC-SHA512, 80-bit / 32-bit tags
const SuiteProfile kAes128Ctr{0x01, 0x03}; // AES-128-CTR+HMAC-SHA256, 80-bit / 32-bit tags
const SuiteProfile kGcm{0x05, 0x04};       // GCM, full 128-bit tags: AES-256 video, AES-128 audio

// The base key is HKDF input keying material, for which RFC 9605 sets no length. The suites from
// 0x05 up derive AES-256 keys, so they are given 32 bytes of it: a 16-byte base key is accepted and
// stretched through HKDF, which silently gives 128-bit security on a suite named for 256.
size_t baseKeySize(uint16_t suite) { return suite >= 0x05 ? 32 : 16; }
const char *kCname = "sframe-offer-answer";
const uint8_t kPayloadType = 96;

// Two further video codecs, for the cases that give the answerer a real choice. Narrowing a codec
// list is the application's job: reciprocate() copies every a=rtpmap the offer carried into the
// answer, and processLocalDescription then takes the answer's m-line verbatim from the track
// description ("Prefer local description"), so nothing in the library ever removes a codec the
// answerer will not use. The answerer below narrows to VP8 -- deliberately neither the first codec
// offered nor the last -- so an offerer that guessed from its own offer rather than reading the
// answer would guess wrong in either direction.
const uint8_t kVp8PayloadType = 98;
const uint8_t kAv1PayloadType = 100;

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
	// False builds the m-line without a=sframe, standing in for a peer that does not support
	// SFrame at all -- distinct from declinedMids, where the offer asks and the answer refuses.
	bool offerSFrame = true;
};

// How the two ends build their handler chains, for the cases that exercise codec negotiation.
//
// A send chain built before the answer arrives has to assume two things it cannot yet know: which
// codec was accepted, and whether a=sframe survived. An application that lets either vary has to
// defer the chain until the answer has been applied, which is what deferSendChain does here.
struct ChainPlan {
	// Offer VP8 and AV1 alongside H.264, so the answerer has something to narrow.
	bool multipleVideoCodecs = false;
	// Payload type the answerer narrows its answer down to, or 0 to answer with the whole offered
	// list, which is what the library does if the application leaves it alone.
	int narrowAnswerTo = 0;
	// Offerer builds its send chain only once the answer has been applied, reading the negotiated
	// payload type and a=sframe out of it, rather than installing a chain before
	// setLocalDescription().
	bool deferSendChain = false;
};

binary sessionKey(size_t size) {
	binary key(size);
	for (size_t i = 0; i < key.size(); ++i)
		key[i] = std::byte(0x40 + i);
	return key;
}

// Answers for the one key generation this test suite uses, and records which generations it was
// asked about. Ratcheting does not change the generation, so that stays at one entry across a
// ratchet -- which is the point of the provider never seeing a ratchet step.
class SessionKeyProvider final : public SFrameReceiveKeyProvider {
public:
	explicit SessionKeyProvider(uint16_t suite = kAes256Ctr.video, uint8_t ratchetStepBits = 0,
	                            bool perSSRC = true)
	    : SFrameReceiveKeyProvider(suite, ratchetStepBits, perSSRC) {
		addKey(uint64_t(1) << ratchetStepBits, SFrameReceiveKey{sessionKey(baseKeySize(suite))});
	}

	optional<SFrameReceiveKey> receiveKey(uint64_t kid) const override {
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mRequested.insert(impl::sframe::KeyGenerationFromKid(kid, ratchetStepBits()));
		}
		return SFrameReceiveKeyProvider::receiveKey(kid);
	}

	std::set<uint64_t> requested() const {
		std::lock_guard<std::mutex> lock(mMutex);
		return mRequested;
	}

private:
	mutable std::mutex mMutex;
	mutable std::set<uint64_t> mRequested;
};

// perSsrc must match the receiving provider. Turning it off is also what puts the provider on the
// shared-key path: with no per-SSRC derivation every track sends under the same key, so the
// provider hands them all one encoder rather than one each.
shared_ptr<SFrameSendKeyProvider> makeSendProvider(uint16_t suite = kAes256Ctr.video,
                                                   uint16_t ratchetPeriod = 0,
                                                   uint8_t ratchetStepBits = 0,
                                                   bool perSsrc = true) {
	return std::make_shared<SFrameSendKeyProvider>(
	    suite, ratchetStepBits, ratchetPeriod, perSsrc,
	    SFrameSendKey{sessionKey(baseKeySize(suite)), uint64_t(1) << ratchetStepBits});
}

// Base counter for a frame, distinct per track so a crossed wire is visible. Spaced far enough
// apart that no two frames on any track can produce overlapping runs.
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

Description::Media makeMedia(const TrackSpec &spec, SSRC ssrc, const ChainPlan &plan) {
	if (spec.kind == Kind::Video) {
		Description::Video video(spec.mid, Description::Direction::SendOnly);
		video.addH264Codec(kPayloadType);
		if (plan.multipleVideoCodecs) {
			// In preference order, H.264 first, so the answerer's choice is visibly its own and not
			// simply the head of the list.
			video.addVP8Codec(kVp8PayloadType);
			video.addAV1Codec(kAv1PayloadType);
		}
		video.addSSRC(ssrc, kCname);
		if (spec.offerSFrame)
			video.addSFrame();
		return video;
	}

	Description::Audio audio(spec.mid, Description::Direction::SendOnly);
	audio.addOpusCodec(kPayloadType);
	audio.addSSRC(ssrc, kCname);
	if (spec.offerSFrame)
		audio.addSFrame();
	return audio;
}

// Records the inbound RTP payload as it arrived, before anything in the chain decrypts it.
// incomingChain() runs from the tail, and useSFrame() installs the SFrame depacketizer at the head,
// so a handler appended to the tail is the wire's-eye view. Without this nothing in the integration
// suite can tell protected media from plaintext: dropping the encodeFrame() call would leave every
// round-trip green, since the far end would strip descriptors and hand up the same bytes.
class WireRecorder final : public MediaHandler {
public:
	void incoming(message_vector &messages, const message_callback &send) override {
		for (const auto &m : messages) {
			if (!m || m->type == Message::Control)
				continue;
			size_t hdrSize = 0, payloadEnd = 0;
			if (!impl::sframe::ParseRtpPayload(*m, hdrSize, payloadEnd) || payloadEnd <= hdrSize)
				continue;
			std::lock_guard<std::mutex> lock(mMutex);
			mPayloads.emplace_back(m->begin() + hdrSize, m->begin() + payloadEnd);
		}
		MediaHandler::incoming(messages, send);
	}

	std::vector<binary> payloads() const {
		std::lock_guard<std::mutex> lock(mMutex);
		return mPayloads;
	}

private:
	mutable std::mutex mMutex;
	std::vector<binary> mPayloads;
};

struct Outcome {
	bool sFrameNegotiated = false; // a=sframe still on the offerer's track description

	// a=sframe as the answerer actually wrote it, from the offerer's remote description. Distinct
	// from the above: for an m-line that never offered the attribute, the offerer's own track says
	// "unset" no matter what came back, so only this can catch an answer that added it.
	bool answerHasSFrame = false;
	std::vector<binary> sent;
	std::vector<binary> received;     // in arrival order
	std::vector<binary> rtcpReceived; // Control messages, in arrival order
	std::vector<binary> wirePayloads; // RTP payloads as they arrived, before decryption
	size_t trackIndex = 0;

	// The payload type the offerer packetized with, and the payload types the far end actually saw
	// on the wire. Both are needed to check codec narrowing: inbound packets are routed to a Track
	// by SSRC and nothing on the receive path filters on payload type, so a round-trip would still
	// pass if the offerer had packetized for a codec the answer removed. A depacketizer stamps the
	// payload type it read into FrameInfo, which is what makes the wire value observable here.
	uint8_t sendPayloadType = kPayloadType;
	std::set<uint8_t> receivedPayloadTypes;

	// The payload types the answerer left in its answer, so narrowing is checked rather than
	// assumed.
	std::vector<int> answerPayloadTypes;
};

// Minimal well-formed RTCP packets. Only the header needs to be real: the point is that
// SFrame neither encrypts nor swallows them, so they must arrive byte-identical.
binary makeRtcp(uint8_t packetType, uint8_t count, size_t bodyWords, uint8_t fill) {
	binary pkt(4 + bodyWords * 4, std::byte{fill});
	pkt[0] = std::byte(0x80 | (count & 0x1F)); // V=2, no padding
	pkt[1] = std::byte(packetType);
	pkt[2] = std::byte((bodyWords >> 8) & 0xFF); // length in 32-bit words minus one
	pkt[3] = std::byte(bodyWords & 0xFF);
	return pkt;
}

// Runs a full offer/answer between two peer connections, then sends one frame per track and
// collects what came out the far end.
//
// `declinedMids` names the m-lines the answerer declines SFrame on, standing in for a peer that
// supports it on some m-lines and not others; `keepSFrameHandlerWhenDeclined` leaves the SFrame
// depacketizer installed on those anyway, so declining is driven by the answer rather than by the
// chain being empty. `plan` covers codec negotiation -- see ChainPlan.
std::map<string, Outcome>
runOfferAnswer(const std::vector<TrackSpec> &specs, const std::set<string> &declinedMids,
               uint64_t ratchetPeriod = 0, uint8_t ratchetStepBits = 0, bool sharedKey = false,
               size_t framesPerTrack = kFramesPerTrack,
               std::chrono::milliseconds frameGap = std::chrono::milliseconds(20),
               std::map<string, std::set<uint64_t>> *kidsSeen = nullptr,
               bool keepSFrameHandlerWhenDeclined = false, bool useSessionProvider = false,
               SuiteProfile profile = kAes256Ctr, ChainPlan plan = ChainPlan{}) {
	// A session provider or a shared key is one provider for every m-line, and a provider fixes its
	// suite at construction, so those two modes cannot split the suite by kind.
	const bool oneSuite = useSessionProvider || sharedKey;
	auto suiteFor = [oneSuite, profile](Kind kind) {
		return (oneSuite || kind == Kind::Video) ? profile.video : profile.audio;
	};
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
	std::map<string, std::set<uint8_t>> receivedPtsByMid;
	std::map<string, std::vector<binary>> rtcpByMid;
	std::map<string, Kind> kindByMid;
	for (const auto &spec : specs)
		kindByMid[spec.mid] = spec.kind;

	std::atomic<int> openRemoteTracks{0};
	std::vector<shared_ptr<Track>> remoteTracks;
	std::map<string, shared_ptr<SessionKeyProvider>> providers;
	std::map<string, shared_ptr<WireRecorder>> recorders;
	std::mutex remoteMutex;

	// One provider for the whole session, registered before any remote description is set, so
	// processRemoteDescription applies it to every m-line that negotiated a=sframe and the track
	// callback has nothing to install. This is the shape the receiver example uses.
	shared_ptr<SessionKeyProvider> sessionProvider;
	if (useSessionProvider) {
		sessionProvider =
		    std::make_shared<SessionKeyProvider>(profile.video, ratchetStepBits, !sharedKey);
		pc2.useSFrame(sessionProvider);
	}

	// Written by the track callback when the library installed a handler on an m-line that never
	// offered a=sframe. Guarded by remoteMutex and asserted after the exchange, since a throw from
	// the callback runs on a PeerConnection thread and would be swallowed.
	string sessionProviderLeak;

	pc2.onTrack([&](shared_ptr<Track> track) {
		const string mid = track->mid();
		// Only m-lines that actually offered a=sframe are candidates: installing an SFrame
		// depacketizer on a track that never asked for one now consumes its media rather than
		// passing it through, since the plaintext fallback is gone.
		bool offered = true;
		for (const auto &spec : specs)
			if (spec.mid == mid)
				offered = spec.offerSFrame;
		const bool negotiated = offered && declinedMids.count(mid) == 0;
		const Kind kind = kindByMid.count(mid) ? kindByMid[mid] : Kind::Video;

		// Narrowing the answer to one codec, which is the answerer's job and nobody else's. The
		// offer's whole codec list arrives here intact -- reciprocate() copies every a=rtpmap, and
		// the answer is then built verbatim from this description -- so an answerer that does not
		// remove what it will not use accepts every codec offered and leaves the offerer no way to
		// know which one to send. removeRtpMap() also drops any RTX mapping whose apt names a
		// payload type being removed, so the answer cannot be left with a dangling one.
		if (plan.narrowAnswerTo && kind == Kind::Video) {
			auto desc = track->description();
			for (int pt : desc.payloadTypes())
				if (pt != plan.narrowAnswerTo)
					desc.removeRtpMap(pt);
			track->setDescription(std::move(desc));
		}

		// Enable SFrame on the m-lines that agreed to it, and on declined ones when
		// keepSFrameHandlerWhenDeclined asks; elsewhere a plain depacketizer hands the payload up.
		if (useSessionProvider) {
			if (track->description().hasSFrame()) {
				// Already set up before this callback ran; only the bookkeeping is left.
				std::lock_guard<std::mutex> lock(remoteMutex);
				providers[mid] = sessionProvider;
			} else {
				// The offer never asked for SFrame, so the session provider must not have been
				// applied here. Checked before installing anything, because this callback replaces
				// the track's handler: without the check, a library that wrongly applied the
				// session provider to every incoming track would be masked by the line below, and
				// the bug -- every plain m-line in a mixed session silently dropping its media --
				// is exactly what this test exists to catch.
				{
					std::lock_guard<std::mutex> lock(remoteMutex);
					if (track->getMediaHandler())
						sessionProviderLeak += " mid=" + mid;
				}

				// It needs an ordinary depacketizer, exactly as if SFrame did not exist.
				track->setMediaHandler(std::make_shared<RtpDepacketizer>(
				    kind == Kind::Video ? RtpPacketizer::VideoClockRate : 48000));
			}
		} else if (negotiated || keepSFrameHandlerWhenDeclined) {
			auto provider =
			    std::make_shared<SessionKeyProvider>(suiteFor(kind), ratchetStepBits, !sharedKey);
			if (negotiated) {
				std::lock_guard<std::mutex> lock(remoteMutex);
				providers[mid] = provider;
			}
			// One call picks the depacketizer for the track's kind and keeps a=sframe alive. The
			// audio rate is passed explicitly rather than left to the description lookup.
			track->useSFrame(provider, kind == Kind::Video ? nullopt : optional<uint32_t>(48000));
		} else {
			// SFrame was declined on this m-line, so the negotiated codec's own depacketizer takes
			// the stage SFrame would have occupied. Where a codec was actually chosen it has to be
			// that codec's, since the packetizer on the other end is that codec's too.
			if (plan.narrowAnswerTo == kVp8PayloadType && kind == Kind::Video)
				track->setMediaHandler(std::make_shared<VP8RtpDepacketizer>());
			else
				track->setMediaHandler(std::make_shared<RtpDepacketizer>(
				    kind == Kind::Video ? RtpPacketizer::VideoClockRate : 48000));
		}

		{
			auto recorder = std::make_shared<WireRecorder>();
			track->chainMediaHandler(recorder);
			std::lock_guard<std::mutex> lock(remoteMutex);
			recorders[mid] = std::move(recorder);
		}

		// The offer's a=sframe reaches this callback, and survives into the answer only because
		// useSFrame() was called above. Where it was not, PeerConnection strips it and the
		// answer declines -- nothing here has to ask for that, and that is the path under test.
		// A mid in declinedMids that did enable SFrame has to decline explicitly, which is what
		// an application refusing SFrame on a track it could have protected would do.
		if (!negotiated && (keepSFrameHandlerWhenDeclined || useSessionProvider)) {
			auto desc = track->description();
			desc.removeSFrame();
			track->setDescription(std::move(desc));
		}

		// A depacketizer stamps frameInfo on what it produces, and impl::Track delivers
		// those through the frame callback -- onMessage() only ever sees messages without
		// frameInfo, so it would never fire here.
		track->onFrame(
		    [&mutex, &receivedByMid, &receivedPtsByMid, mid](binary data, FrameInfo info) {
			    // Appended in arrival order: the comparison below is order-sensitive. The payload
			    // type is the one the depacketizer read off the wire, so it records what the far
			    // end actually packetized for rather than what it was asked to.
			    std::lock_guard<std::mutex> lock(mutex);
			    receivedPtsByMid[mid].insert(info.payloadType);
			    receivedByMid[mid].push_back(std::move(data));
		    });

		// RTCP carries no frameInfo, so it arrives here rather than through onFrame.
		track->onMessage(
		    [&mutex, &rtcpByMid, mid](binary data) {
			    std::lock_guard<std::mutex> lock(mutex);
			    rtcpByMid[mid].push_back(std::move(data));
		    },
		    [](string) {});

		{
			std::lock_guard<std::mutex> lock(remoteMutex);
			remoteTracks.push_back(track);
		}
		openRemoteTracks++;
	});

	// Offerer: every track gets an SFrame packetizer. Negotiation decides whether it
	// actually applies SFrame, which is the behaviour under test.
	//
	// Installing it here, before the offer goes out, is only safe because of two properties of
	// these m-lines. Each offers a single codec, so the payload type cannot change in the answer;
	// and SFramePerFrameRtpPacketizer is codec-agnostic -- it encrypts the whole frame and splits
	// the result into MTU-sized chunks -- so it stays correct whatever the answer picks. Where
	// either property fails, the chain has to wait for the answer, which is what
	// ChainPlan::deferSendChain does below.
	std::vector<shared_ptr<Track>> tracks;
	std::vector<SSRC> trackSsrcs;
	auto sharedProvider = sharedKey ? makeSendProvider(profile.video, ratchetPeriod,
	                                                   ratchetStepBits, /*perSsrc=*/false)
	                                : nullptr;
	std::map<string, std::vector<binary>> sentByMid;
	SSRC ssrc = 0x1000;
	for (const auto &spec : specs) {
		const SSRC trackSsrc = ssrc++;
		trackSsrcs.push_back(trackSsrc);
		auto track = pc1.addTrack(makeMedia(spec, trackSsrc, plan));
		expect(track->description().hasSFrame() == spec.offerSFrame,
		       "offerer track mid=" + spec.mid + ": a=sframe does not match the spec");

		if (!plan.deferSendChain) {
			const uint32_t clockRate =
			    spec.kind == Kind::Video ? RtpPacketizer::VideoClockRate : 48000;
			auto rtpConfig = std::make_shared<RtpPacketizationConfig>(trackSsrc, kCname,
			                                                          kPayloadType, clockRate);
			if (!spec.offerSFrame) {
				// This m-line never asked for protection, so it is an ordinary track. It gets the
				// ordinary packetizer: an SFrame one here would encrypt media the description does
				// not claim is encrypted, and there is no longer a fallback that would let it
				// pass plaintext through.
				track->setMediaHandler(std::make_shared<RtpPacketizer>(rtpConfig));
			} else if (sharedKey) {
				// No per-SSRC derivation means one key for every track, so the provider hands
				// them all one encoder: separate ones would each start at ctrStart and reuse
				// nonces.
				track->setMediaHandler(
				    std::make_shared<SFramePerFrameRtpPacketizer>(rtpConfig, sharedProvider));
			} else {
				track->setMediaHandler(std::make_shared<SFramePerFrameRtpPacketizer>(
				    rtpConfig,
				    makeSendProvider(suiteFor(spec.kind), ratchetPeriod, ratchetStepBits)));
			}
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

	// Deferred send chain, built only now that the answer has been applied. An application has to
	// work this way as soon as it lets the answer decide either the codec or a=sframe, and nothing
	// is lost by waiting: Track::isOpen() stays false until the DTLS-SRTP transport exists, so no
	// frame could have gone out before this point anyway.
	std::vector<uint8_t> sendPayloadType(tracks.size(), kPayloadType);
	if (plan.deferSendChain) {
		expect(!sharedKey, "the deferred path builds a send provider per track, so it cannot also "
		                   "share one key across them");

		auto remote = pc1.remoteDescription();
		expect(remote.has_value(),
		       "the offerer has no remote description, so there was no answer to defer to");
		const Description &answer = *remote;

		for (size_t i = 0; i < tracks.size(); ++i) {
			// The answer's own m-line is the only place the negotiated codec list appears.
			// processRemoteDescription() propagates just RTX and SFrame changes onto an existing
			// track, so tracks[i]->description() still lists every codec the offer carried and is
			// no help here.
			const Description::Media *answered = nullptr;
			for (int m = 0; m < answer.mediaCount(); ++m) {
				auto entry = answer.media(m);
				if (auto media = std::get_if<const Description::Media *>(&entry))
					if (*media && (*media)->mid() == specs[i].mid)
						answered = *media;
			}
			expect(answered != nullptr, "mid=" + specs[i].mid + " is missing from the answer");

			// The answerer's first non-RTX payload type is the codec it chose: SDP lists formats in
			// the sender's preference order, and for an answer that sender is the answerer.
			int negotiated = 0;
			for (int pt : answered->payloadTypes()) {
				if (!answered->hasPayloadType(pt))
					continue; // a static payload type carries no a=rtpmap (RFC 4566)
				const auto *map = answered->rtpMap(pt);
				if (map->format == "rtx" || map->format == "RTX")
					continue;
				negotiated = pt;
				break;
			}
			expect(negotiated != 0, "mid=" + specs[i].mid + ": the answer names no codec to send");
			sendPayloadType[i] = uint8_t(negotiated);

			const uint32_t clockRate =
			    specs[i].kind == Kind::Video ? RtpPacketizer::VideoClockRate : 48000;
			auto rtpConfig = std::make_shared<RtpPacketizationConfig>(
			    trackSsrcs[i], kCname, uint8_t(negotiated), clockRate);

			if (answered->hasSFrame()) {
				tracks[i]->setMediaHandler(std::make_shared<SFramePerFrameRtpPacketizer>(
				    rtpConfig,
				    makeSendProvider(suiteFor(specs[i].kind), ratchetPeriod, ratchetStepBits)));
			} else {
				// SFrame was declined, so the negotiated codec's own packetizer takes its place. An
				// upfront chain cannot express this: it would still be encrypting, into a far end
				// that now has a codec depacketizer.
				expect(negotiated == kVp8PayloadType,
				       "mid=" + specs[i].mid +
				           ": the declined path only knows how to build a VP8 send chain, and the "
				           "answer chose payload type " +
				           to_string(negotiated));
				tracks[i]->setMediaHandler(std::make_shared<VP8RtpPacketizer>(rtpConfig));
			}
		}
	}

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

	// Send and RTCP failures, and track-open mismatches, collected by the send loop below.
	string sendError;
	std::map<string, std::vector<binary>> sentRtcpByMid;

	for (size_t i = 0; i < tracks.size(); ++i) {
		// Offered a=sframe and did not get it back: the library stops the transceiver, so every
		// send below is expected to throw and the track is expected to be closed.
		const bool stopped = declinedMids.count(specs[i].mid) != 0;

		for (size_t f = 0; f < framesPerTrack; ++f) {
			const size_t size = specs[i].kind == Kind::Video ? kVideoFrameSize : audioFrameSize(f);
			auto frame = makeFrame(size, frameBase(i, f));
			sentByMid[specs[i].mid].push_back(frame);

			FrameInfo info(uint32_t(3000 * (f + 1)));
			info.payloadType = sendPayloadType[i];
			try {
				tracks[i]->sendFrame(binary(frame), info);
			} catch (const std::exception &e) {
				// A declined m-line is stopped, so its track is closed and sending throws. That is
				// the behaviour under test, not a failure.
				if (!stopped)
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
			if (!stopped)
				sendError += " mid=" + specs[i].mid + " rtcp app send threw: " + e.what() + ";";
		}
		this_thread::sleep_for(std::chrono::milliseconds(20));

		// A declined m-line must be closed, and one that kept SFrame must be open: asserting both
		// directions keeps this from passing whichever way the library behaves.
		if (tracks[i]->isOpen() == stopped)
			sendError += " mid=" + specs[i].mid + (stopped ? " local track still open after the "
			                                                 "answer declined SFrame;"
			                                               : " local track not open;");
	}
	{
		std::lock_guard<std::mutex> lock(remoteMutex);
		for (const auto &t : remoteTracks)
			if (!t->isOpen() && declinedMids.count(t->mid()) == 0)
				sendError += " mid=" + t->mid() + " REMOTE track not open;";
	}
	expect(sendError.empty(), "send failed:" + sendError);
	{
		std::lock_guard<std::mutex> lock(remoteMutex);
		expect(sessionProviderLeak.empty(),
		       "the library installed a receive handler on an m-line that never offered a=sframe:" +
		           sessionProviderLeak +
		           " -- a session provider must leave those tracks alone, or every plain m-line in "
		           "a mixed session drops its media");
	}

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
	// The answerer's final view of each m-line, which is what its answer carried.
	std::map<string, std::vector<int>> answerPtsByMid;
	{
		std::lock_guard<std::mutex> lock(remoteMutex);
		for (const auto &t : remoteTracks)
			answerPtsByMid[t->mid()] = t->description().payloadTypes();
	}

	// What the answerer actually put on the wire, read off the offerer's remote description. The
	// offerer's own track description is not a substitute: for an m-line that never offered
	// a=sframe it reports "unset" whatever the answerer did, so an assertion against it holds even
	// if the answer wrongly asserted the attribute.
	std::map<string, bool> answerHasSFrameByMid;
	if (const auto remote = pc1.remoteDescription()) { // const picks the const media() overload
		for (int i = 0; i < remote->mediaCount(); ++i) {
			auto entry = remote->media(i);
			if (auto media = std::get_if<const Description::Media *>(&entry))
				answerHasSFrameByMid[(*media)->mid()] = (*media)->hasSFrame();
		}
	}
	for (size_t i = 0; i < tracks.size(); ++i) {
		const string &mid = specs[i].mid;
		Outcome outcome;
		outcome.trackIndex = i;
		outcome.sFrameNegotiated = tracks[i]->description().hasSFrame();
		outcome.answerHasSFrame = answerHasSFrameByMid.count(mid) ? answerHasSFrameByMid[mid]
		                                                          : outcome.sFrameNegotiated;
		outcome.sendPayloadType = sendPayloadType[i];
		if (answerPtsByMid.count(mid))
			outcome.answerPayloadTypes = answerPtsByMid[mid];

		outcome.sent = sentByMid[mid];
		std::lock_guard<std::mutex> lock(mutex);
		if (receivedByMid.count(mid))
			outcome.received = receivedByMid[mid];
		if (receivedPtsByMid.count(mid))
			outcome.receivedPayloadTypes = receivedPtsByMid[mid];
		if (rtcpByMid.count(mid))
			outcome.rtcpReceived = rtcpByMid[mid];
		if (auto it = recorders.find(mid); it != recorders.end())
			outcome.wirePayloads = it->second->payloads();
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

// Whether the plaintext ever appeared on the wire. This is the only thing in the integration suite
// that can tell protected media from unprotected: every other assertion compares plaintext in
// against plaintext out, which a build that framed but never encrypted would satisfy.
//
// @param expectProtected true if a=sframe was negotiated, so no sent frame's body may appear in any
//                        inbound RTP payload; false to require the opposite, which is what keeps
//                        this honest -- an assertion that can only ever pass proves nothing.
void checkWireProtection(const std::map<string, Outcome> &outcomes, const string &mid,
                         bool expectProtected) {
	auto it = outcomes.find(mid);
	expect(it != outcomes.end(), "no outcome recorded for mid=" + mid);
	const Outcome &outcome = it->second;

	expect(!outcome.wirePayloads.empty(),
	       "mid=" + mid + ": nothing was recorded from the wire, so this check is vacuous");
	expect(!outcome.sent.empty(), "mid=" + mid + ": no frames were sent");

	// A frame is fragmented across packets, so look for any sent frame's opening run inside any
	// payload rather than for a whole frame. 16 bytes is far beyond coincidence and short enough to
	// sit inside one fragment.
	const size_t probe = 16;
	size_t found = 0;
	for (const auto &frame : outcome.sent) {
		if (frame.size() < probe)
			continue;
		const binary needle(frame.begin(), frame.begin() + probe);
		for (const auto &payload : outcome.wirePayloads) {
			if (payload.size() < needle.size())
				continue;
			if (std::search(payload.begin(), payload.end(), needle.begin(), needle.end()) !=
			    payload.end()) {
				++found;
				break;
			}
		}
	}

	if (expectProtected)
		expect(found == 0,
		       "mid=" + mid + ": " + to_string(found) +
		           " sent frame(s) appeared in the clear on the wire while a=sframe was "
		           "negotiated, so the media was framed but not encrypted");
	else
		expect(found > 0, "mid=" + mid +
		                      ": no sent frame was found on the wire even though SFrame was "
		                      "declined, so this check is not looking where it thinks it is");
}

// An m-line that offered a=sframe and did not get it back is stopped, not downgraded: the track
// closes and no media crosses. Asserting an absence proves little on its own, so the mixed-offer and
// partial-answer tests pair this with a sibling m-line that did negotiate.
void checkStopped(const std::map<string, Outcome> &outcomes, const string &mid) {
	auto it = outcomes.find(mid);
	expect(it != outcomes.end(), "no outcome recorded for mid=" + mid);
	const Outcome &outcome = it->second;

	expect(!outcome.sFrameNegotiated,
	       "mid=" + mid + ": a=sframe survived on a track whose answer declined it");
	expect(outcome.received.empty(),
	       "mid=" + mid + ": " + to_string(outcome.received.size()) +
	           " frames arrived on a stopped track -- the decline downgraded it to plaintext "
	           "instead of stopping it");
	expect(outcome.wirePayloads.empty(),
	       "mid=" + mid + ": " + to_string(outcome.wirePayloads.size()) +
	           " RTP payloads reached the wire on a stopped track");
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
			arrived +=
			    " base=" + to_string(readCounter(frame, 0)) + "(" + to_string(frame.size()) + "B)";
		}
		string wanted;
		for (size_t i = 0; i < outcome.sent.size(); ++i)
			wanted += " base=" + to_string(frameBase(outcome.trackIndex, i)) + "(" +
			          to_string(outcome.sent[i].size()) + "B)";
		expect(false, "mid=" + mid + ": received " + to_string(outcome.received.size()) +
		                  " frames, sent " + to_string(outcome.sent.size()) +
		                  "; arrived:" + arrived + "; expected:" + wanted);
	}

	for (size_t i = 0; i < outcome.sent.size(); ++i) {
		const binary &sent = outcome.sent[i];
		const binary &received = outcome.received[i];

		expect(received.size() == sent.size(), "mid=" + mid + " frame " + to_string(i) +
		                                           ": received " + to_string(received.size()) +
		                                           " bytes, sent " + to_string(sent.size()));

		// The counter run says more than a plain comparison: a wrong starting value means
		// the frames themselves arrived out of order, and a break part way through means
		// the fragments of one frame did.
		const string problem = describeSequence(received, frameBase(outcome.trackIndex, i));
		expect(problem.empty(), "mid=" + mid + " frame " + to_string(i) + ": " + problem);

		expect(received == sent,
		       "mid=" + mid + " frame " + to_string(i) + ": bytes differ from those sent");
	}
}

// Checks that the codec the answerer chose is the codec the offerer actually packetized for.
// Neither half follows from a successful round-trip: inbound packets reach a Track by SSRC and
// nothing on the receive path filters on payload type, so an offerer still sending the offer's own
// first choice would deliver its frames just the same and the mismatch would go unnoticed until a
// real decoder saw them.
void checkNegotiatedCodec(const std::map<string, Outcome> &outcomes, const string &mid,
                          uint8_t expectedPayloadType) {
	auto it = outcomes.find(mid);
	expect(it != outcomes.end(), "no outcome recorded for mid=" + mid);
	const Outcome &outcome = it->second;

	string answered;
	for (int pt : outcome.answerPayloadTypes)
		answered += " " + to_string(pt);
	expect(outcome.answerPayloadTypes.size() == 1,
	       "mid=" + mid + ": the answer carries " + to_string(outcome.answerPayloadTypes.size()) +
	           " payload types (" + answered + " ), so it was never narrowed to one codec");
	expect(outcome.answerPayloadTypes.front() == int(expectedPayloadType),
	       "mid=" + mid + ": the answer chose payload type " +
	           to_string(outcome.answerPayloadTypes.front()) + ", expected " +
	           to_string(expectedPayloadType));

	expect(outcome.sendPayloadType == expectedPayloadType,
	       "mid=" + mid + ": the offerer packetized for payload type " +
	           to_string(outcome.sendPayloadType) + " but the answer chose " +
	           to_string(expectedPayloadType) + ", so the send chain ignored the answer");

	// What actually crossed the wire, as read back off the RTP headers by the depacketizer.
	string seen;
	for (uint8_t pt : outcome.receivedPayloadTypes)
		seen += " " + to_string(pt);
	expect(outcome.receivedPayloadTypes.size() == 1 &&
	           *outcome.receivedPayloadTypes.begin() == expectedPayloadType,
	       "mid=" + mid + ": the far end saw payload types" + seen + ", expected only " +
	           to_string(expectedPayloadType));
}

// The RTP clock rate the track's SFrame depacketizer was built with, read back from what it stamps
// on a decoded frame: RtpDepacketizer::createFrameInfo() fills FrameInfo::timestampSeconds by
// dividing the RTP timestamp by that rate, which is the only place the value is observable from
// outside the handler.
//
// The probe frame is packetized under a matching send provider and pushed through the track's own
// chain, so a rate is only ever read off a frame that actually decrypted.
uint32_t observedClockRate(const shared_ptr<Track> &track, SSRC ssrc, uint8_t payloadType) {
	auto head = track->getMediaHandler();
	expect(head != nullptr, "the track has no media handler, so useSFrame() installed nothing");

	// Divides exactly by every rate these cases use, so the seconds come back free of rounding.
	const uint32_t timestamp = 720000;

	// The send-side rate only converts FrameInfo::timestampSeconds into an RTP timestamp, and the
	// frame below carries the timestamp itself, so it cannot influence what the receiver stamps.
	auto rtpConfig =
	    std::make_shared<RtpPacketizationConfig>(ssrc, kCname, payloadType, /*clockRate=*/48000);
	SFramePerFrameRtpPacketizer packetizer(rtpConfig, makeSendProvider());

	auto frame = makeFrame(kAudioFrameSizeSmall, 0x5A000000);
	auto frameInfo = std::make_shared<FrameInfo>(timestamp);
	frameInfo->payloadType = payloadType;
	message_vector packets{make_message(binary(frame), frameInfo)};
	packetizer.outgoing(packets, [](message_ptr) {});

	binary decoded;
	optional<double> seconds;
	for (auto &packet : packets) {
		message_vector one{packet};
		head->incomingChain(one, [](message_ptr) {});
		for (auto &out : one)
			if (out && out->type != Message::Control && out->frameInfo) {
				decoded.assign(out->begin(), out->end());
				if (out->frameInfo->timestampSeconds)
					seconds = out->frameInfo->timestampSeconds->count();
			}
	}

	expect(decoded.size() == frame.size() &&
	           std::equal(frame.begin(), frame.end(), decoded.begin()),
	       "the probe frame did not decrypt, so there is no clock rate to read off it");
	expect(seconds.has_value() && *seconds > 0,
	       "the depacketizer stamped no timestamp in seconds, so it holds no clock rate at all");
	return uint32_t(std::llround(double(timestamp) / *seconds));
}

} // namespace

// Track::incoming delivers one message per call, so RTCP always reaches the handler chain
// in a batch of its own with no media alongside -- exactly the case a fail-closed receive
// path gets wrong by clearing the whole batch.
void checkRtcpPassedThrough(const std::map<string, Outcome> &outcomes, const string &mid) {
	auto it = outcomes.find(mid);
	expect(it != outcomes.end(), "no outcome recorded for mid=" + mid);
	const auto &got = it->second.rtcpReceived;

	expect(!got.empty(),
	       "mid=" + mid + ": no RTCP reached the far end, so the SFrame handlers ate it");

	bool sawApp = false;
	for (const auto &pkt : got) {
		expect(pkt.size() >= 4, "mid=" + mid + ": truncated RTCP packet");
		expect((uint8_t(pkt[0]) >> 6) == 2, "mid=" + mid + ": RTCP version mangled");
		if (uint8_t(pkt[1]) != 204)
			continue; // SR/RR/feedback the stack generates on its own
		sawApp = true;
		// Encryption would change every byte, so an intact body proves it passed through.
		expect(uint8_t(pkt.back()) == 0xC3, "mid=" + mid + ": APP body was modified in transit");
	}
	expect(sawApp, "mid=" + mid + ": the APP packet did not arrive");
}

TestResult test_sframe_rtcp_passthrough() {
	try {
		// One of each kind: the audio and video receive paths are separate code.
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {});
		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);
		checkWireProtection(outcomes, "0", /*expectProtected=*/true);
		checkWireProtection(outcomes, "1", /*expectProtected=*/true);
		checkRtcpPassedThrough(outcomes, "0");
		checkRtcpPassedThrough(outcomes, "1");
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
	return TestResult(true);
}

// A track that never offered SFrame still has to carry RTCP.
// The counterfactual for checkWireProtection(): a probe that never finds plaintext proves nothing
// unless it can find it when there is some. It cannot come from a declined m-line any more -- those
// are stopped and carry nothing -- so it comes from m-lines that never offered a=sframe at all,
// which is the only remaining way for this library to put unprotected media on the wire.
TestResult test_sframe_rtcp_passthrough_unprotected() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video, /*offerSFrame=*/false},
		                                {"1", Kind::Audio, /*offerSFrame=*/false}},
		                               {});
		checkWireProtection(outcomes, "0", /*expectProtected=*/false);
		checkWireProtection(outcomes, "1", /*expectProtected=*/false);
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
		                               /*sharedKey=*/true, kFramesPerTrack,
		                               std::chrono::milliseconds(20), /*kidsSeen=*/nullptr,
		                               /*keepSFrameHandlerWhenDeclined=*/false,
		                               /*useSessionProvider=*/false, kAes128Ctr);
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

// Single video m-line, answer declines. The m-line is stopped rather than downgraded: the track
// closes and nothing reaches the wire.
TestResult test_sframe_video_answer_no() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}}, {"0"});
		checkStopped(outcomes, "0");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Same case, but the receiver leaves its SFramePerFrameVideoRtpDepacketizer installed instead of
// swapping in a plain one -- what an application that builds its chain before seeing the answer
// ends up with. Keeping the handler changes nothing: the m-line is still stopped and still carries
// no media.
TestResult test_sframe_video_declined_keeps_depacketizer() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}}, {"0"}, /*ratchetPeriod=*/0,
		                               /*ratchetStepBits=*/0, /*sharedKey=*/false, kFramesPerTrack,
		                               std::chrono::milliseconds(20),
		                               /*kidsSeen=*/nullptr,
		                               /*keepSFrameHandlerWhenDeclined=*/true);
		checkStopped(outcomes, "0");
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
		checkStopped(outcomes, "0");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Description::hasSFrame() sweeps the m-lines, so an application can tell from the offer it
// received whether to register a session-wide key provider before setRemoteDescription().
TestResult test_sframe_description_has_sframe() {
	try {
		Description offer("", Description::Type::Offer);
		expect(!offer.hasSFrame(), "an empty description should not report SFrame");

		Description::Video plain("0", Description::Direction::SendOnly);
		plain.addH264Codec(kPayloadType);
		offer.addMedia(plain);
		expect(!offer.hasSFrame(), "an m-line without a=sframe should not report SFrame");

		Description::Audio protectedAudio("1", Description::Direction::SendOnly);
		protectedAudio.addOpusCodec(111);
		protectedAudio.addSFrame();
		offer.addMedia(protectedAudio);
		expect(offer.hasSFrame(), "a=sframe on any m-line should report SFrame");

		// Valued forms count too, for the same reason Media::hasSFrame() accepts them: a peer
		// sending parameters is asserting support, not declining.
		Description valued("", Description::Type::Offer);
		Description::Video withParams("0", Description::Direction::SendOnly);
		withParams.addH264Codec(kPayloadType);
		withParams.addAttribute("sframe:profile=1");
		valued.addMedia(withParams);
		expect(valued.hasSFrame(), "a valued a=sframe was missed at the session level");

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

// An m-line's format list carries static payload types with no a=rtpmap (RFC 4566), and
// Description::Media::rtpMap() throws for those. Deriving the audio clock rate has to skip them:
// throwing out of here aborts setRemoteDescription() part way through, so onTrack never fires,
// the later m-lines are never processed and no answer is generated.
TestResult test_sframe_audio_static_payload_type() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Media mixed("m=audio 9 UDP/TLS/RTP/SAVPF 0 111\r\n"
		                         "a=mid:0\r\n"
		                         "a=sendrecv\r\n"
		                         "a=sframe\r\n"
		                         "a=rtpmap:111 opus/48000/2\r\n");
		expect(mixed.hasSFrame(), "the m-line lost a=sframe");
		expect(mixed.payloadTypes().size() == 2, "the m-line lost a payload type");
		expect(!mixed.hasPayloadType(0), "payload type 0 should carry no rtpmap");

		auto track = pc.addTrack(mixed);
		track->useSFrame(std::make_shared<SessionKeyProvider>());
		expect(std::dynamic_pointer_cast<SFramePerFrameAudioRtpDepacketizer>(
		           track->getMediaHandler()) != nullptr,
		       "useSFrame() did not install the audio SFrame depacketizer");

		// No rtpmap at all leaves the rate genuinely underivable, and that still has to throw.
		Description::Media bare("m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
		                        "a=mid:1\r\n"
		                        "a=sendrecv\r\n"
		                        "a=sframe\r\n");
		auto bareTrack = pc.addTrack(bare);
		bool threw = false;
		try {
			bareTrack->useSFrame(std::make_shared<SessionKeyProvider>());
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		expect(threw, "an m-line with no clock rate at all should still throw");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Replacing the chain after useSFrame() declines SFrame rather than leaving the answer asserting
// protection nothing provides. Installing a codec depacketizer in the track callback is the taught
// pattern, so the answer must follow what is actually in the chain.
TestResult test_sframe_replaced_chain_declines() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered("m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		                           "a=mid:0\r\n"
		                           "a=sendrecv\r\n"
		                           "a=sframe\r\n"
		                           "a=rtpmap:96 H264/90000\r\n");
		auto track = pc.addTrack(offered);
		track->useSFrame(std::make_shared<SessionKeyProvider>());
		expect(track->getMediaHandler() && track->getMediaHandler()->appliesSFrame(),
		       "useSFrame() did not put an SFrame handler in the chain");

		// The application replaces the whole chain with its own codec depacketizer.
		track->setMediaHandler(std::make_shared<H264RtpDepacketizer>());
		expect(!track->getMediaHandler()->appliesSFrame(),
		       "the chain still claims to apply SFrame after it was replaced, so an answer would "
		       "assert protection that nothing provides");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// useSFrame() must keep the chain the application already installed. SFrame goes in front of it,
// so on the way in RTX is unwrapped and RTCP is handled before the descriptor byte is read, and
// every handler still gets the negotiated description. Dropping any of it would silently disable
// RTX and the RTCP handlers, leaving an answer that still asserts a=sframe.
TestResult test_sframe_preserves_existing_chain() {
	try {
		Configuration config;
		PeerConnection pc(config);

		const SSRC ssrc = 0x0C0C0C0C;
		const SSRC rtxSsrc = 0x0D0D0D0D;
		const uint8_t rtxPayloadType = kPayloadType + 1;

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addRtxCodec(rtxPayloadType, kPayloadType, 90000);
		offered.addSSRC(ssrc, kCname);
		offered.addRtxSSRC(ssrc, rtxSsrc, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		// The application's own chain, installed before SFrame as an application would.
		std::atomic<int> pliCount{0};
		std::atomic<int> appCount{0};
		auto receiving = std::make_shared<RtcpReceivingSession>();
		auto nack = std::make_shared<RtcpNackResponder>();
		auto pli = std::make_shared<PliHandler>([&pliCount]() { pliCount++; });
		auto app = std::make_shared<RtcpAppHandler>(
		    [&appCount](const RtcpAppName, uint8_t, binary) { appCount++; });
		track->setMediaHandler(receiving);
		track->chainMediaHandler(nack);
		track->chainMediaHandler(pli);
		track->chainMediaHandler(app);

		track->useSFrame(std::make_shared<SessionKeyProvider>());

		// SFrame is at the head, so the answer may still assert a=sframe.
		auto head = track->getMediaHandler();
		expect(head && head->appliesSFrame(),
		       "useSFrame() did not put an SFrame handler at the head of the chain");

		// Everything installed earlier is still reachable behind it.
		std::set<MediaHandler *> present;
		for (auto h = head; h; h = h->next())
			present.insert(h.get());
		expect(present.count(receiving.get()) == 1,
		       "useSFrame() dropped the RtcpReceivingSession, so RTX would never be unwrapped");
		expect(present.count(nack.get()) == 1, "useSFrame() dropped the RtcpNackResponder");
		expect(present.count(pli.get()) == 1, "useSFrame() dropped the PliHandler");
		expect(present.count(app.get()) == 1, "useSFrame() dropped the RtcpAppHandler");

		// The PLI handler still sees a PLI.
		{
			binary pliData(RtcpPli::Size());
			reinterpret_cast<RtcpPli *>(pliData.data())->preparePacket(ssrc);
			message_vector messages{make_message(std::move(pliData), Message::Control)};
			head->incomingChain(messages, [](message_ptr) {});
			expect(pliCount == 1, "the PliHandler did not see the PLI after useSFrame()");
		}

		// The APP handler still sees an RTCP APP packet.
		{
			const RtcpAppName name{'t', 'e', 's', 't'};
			binary appData(RtcpApp::SizeWithData(4));
			reinterpret_cast<RtcpApp *>(appData.data())->preparePacket(ssrc, name, 1, 4);
			message_vector messages{make_message(std::move(appData), Message::Control)};
			head->incomingChain(messages, [](message_ptr) {});
			expect(appCount == 1,
			       "the RtcpAppHandler did not see the APP packet after useSFrame()");
		}

		// An SFrame frame recovered through an RTX retransmission still decrypts, which only
		// holds if media() reached the RtcpReceivingSession and it unwraps ahead of SFrame.
		{
			// The suite's own helper, so the KID matches what SessionKeyProvider registered.
			auto sendProvider = makeSendProvider();
			auto rtpConfig = std::make_shared<RtpPacketizationConfig>(
			    ssrc, kCname, kPayloadType, RtpPacketizer::VideoClockRate);
			SFramePerFrameRtpPacketizer packetizer(rtpConfig, sendProvider);

			binary frame(1600);
			for (size_t i = 0; i < frame.size(); ++i)
				frame[i] = std::byte(0x90 + (i % 7));

			auto frameInfo = std::make_shared<FrameInfo>(9000u);
			frameInfo->payloadType = kPayloadType;
			message_vector packets{make_message(binary(frame), frameInfo)};
			packetizer.outgoing(packets, [](message_ptr) {});
			expect(packets.size() == 2, "expected a two-packet frame to retransmit one of");

			// Build the retransmission with the real responder, as the sending peer would.
			RtcpNackResponder sender;
			sender.media(offered);
			{
				message_vector outgoing = packets;
				sender.outgoing(outgoing, [](message_ptr) {});
			}
			const uint16_t lostSeq =
			    reinterpret_cast<const RtpHeader *>(packets[1]->data())->seqNumber();
			binary nackData(RtcpNack::Size(1));
			auto nackPacket = reinterpret_cast<RtcpNack *>(nackData.data());
			nackPacket->preparePacket(ssrc, 1);
			unsigned int fciCount = 0;
			uint16_t fciPID = lostSeq;
			nackPacket->addMissingPacket(&fciCount, &fciPID, lostSeq);

			message_ptr retransmission;
			message_vector nackMessages{make_message(std::move(nackData), Message::Control)};
			sender.incoming(nackMessages, [&retransmission](message_ptr m) {
				if (m && m->type != Message::Control)
					retransmission = std::move(m);
			});
			expect(retransmission != nullptr, "the NACK produced no retransmission");
			expect(reinterpret_cast<const RtpHeader *>(retransmission->data())->ssrc() == rtxSsrc,
			       "the retransmission should carry the RTX SSRC");

			// The first packet arrives normally, the second only as the retransmission.
			binary decoded;
			for (auto &packet : {packets[0], retransmission}) {
				message_vector one{packet};
				head->incomingChain(one, [](message_ptr) {});
				for (auto &out : one)
					if (out && out->type != Message::Control && out->frameInfo)
						decoded.assign(out->begin(), out->end());
			}
			expect(decoded.size() == frame.size() &&
			           std::equal(frame.begin(), frame.end(), decoded.begin()),
			       "the frame recovered through RTX did not decrypt after useSFrame() rebuilt the "
			       "chain");
		}

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// media-receiver installs a codec depacketizer and then an RtcpReceivingSession, before any
// negotiation. useSFrame() on top of that must keep the RTCP session -- dropping it disables RTX --
// while standing in for the codec depacketizer, which occupies the same stage on the incoming path.
// Keeping both would hand SFrame payloads whose RTP headers the codec handler had already stripped.
TestResult test_sframe_replaces_codec_depacketizer() {
	try {
		Configuration config;
		PeerConnection pc(config);

		const SSRC ssrc = 0x0E0E0E0E;
		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(ssrc, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		// The pattern the media-receiver example teaches.
		auto codec = std::make_shared<H264RtpDepacketizer>();
		auto receiving = std::make_shared<RtcpReceivingSession>();
		track->setMediaHandler(codec);
		track->chainMediaHandler(receiving);

		track->useSFrame(std::make_shared<SessionKeyProvider>());

		auto head = track->getMediaHandler();
		expect(head && head->appliesSFrame(), "useSFrame() did not put SFrame at the head");

		std::set<MediaHandler *> present;
		for (auto h = head; h; h = h->next())
			present.insert(h.get());
		expect(present.count(receiving.get()) == 1,
		       "the RtcpReceivingSession was dropped, so RTX would never be unwrapped");
		expect(present.count(codec.get()) == 0,
		       "the codec depacketizer is still in the chain, so it would strip the RTP headers "
		       "before SFrame ever saw them");

		// And the frames still decrypt through the resulting chain.
		auto rtpConfig = std::make_shared<RtpPacketizationConfig>(ssrc, kCname, kPayloadType,
		                                                          RtpPacketizer::VideoClockRate);
		SFramePerFrameRtpPacketizer packetizer(rtpConfig, makeSendProvider());
		binary frame(400);
		for (size_t i = 0; i < frame.size(); ++i)
			frame[i] = std::byte(0xA0 + (i % 5));
		auto frameInfo = std::make_shared<FrameInfo>(9000u);
		frameInfo->payloadType = kPayloadType;
		message_vector packets{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(packets, [](message_ptr) {});

		binary decoded;
		for (auto &packet : packets) {
			message_vector one{packet};
			head->incomingChain(one, [](message_ptr) {});
			for (auto &out : one)
				if (out && out->type != Message::Control && out->frameInfo)
					decoded.assign(out->begin(), out->end());
		}
		expect(decoded.size() == frame.size() &&
		           std::equal(frame.begin(), frame.end(), decoded.begin()),
		       "the frame did not decrypt after useSFrame() rebuilt the chain around the codec "
		       "depacketizer");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Only depacketizers share SFrame's stage. A sendrecv track needs its own packetizer to send under,
// so useSFrame() installing the receive side must leave it in place.
TestResult test_sframe_keeps_packetizer_on_sendrecv() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0F0F0F0F, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		auto rtpConfig = std::make_shared<RtpPacketizationConfig>(0x0F0F0F0F, kCname, kPayloadType,
		                                                          RtpPacketizer::VideoClockRate);
		auto packetizer =
		    std::make_shared<SFramePerFrameRtpPacketizer>(rtpConfig, makeSendProvider());
		track->setMediaHandler(packetizer);

		track->useSFrame(std::make_shared<SessionKeyProvider>());

		std::set<MediaHandler *> present;
		for (auto h = track->getMediaHandler(); h; h = h->next())
			present.insert(h.get());
		expect(present.count(packetizer.get()) == 1,
		       "useSFrame() dropped the send-side packetizer, so this end would stop sending");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Advertising a=sframe commits this end to encrypting what it sends, so sending with no SFrame
// packetizer in the chain would put plaintext on an m-line the SDP declares end-to-end protected --
// and the component that would normally warn about it is the very one that is missing. The refusal
// lands on the first frame rather than at useSFrame() or at answer time, because a chain is
// legitimately incomplete part way through being built and the deferred model installs the send
// side only once the answer has been read.
TestResult test_sframe_send_without_packetizer_refused() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0F0F0F0F, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		// Receive side only, which is all PeerConnection::useSFrame() and Track::useSFrame()
		// install.
		track->useSFrame(std::make_shared<SessionKeyProvider>());

		bool threw = false;
		try {
			track->sendFrame(binary(64, std::byte{0x01}), FrameInfo(0));
		} catch (const std::logic_error &) {
			threw = true;
		} catch (const std::exception &e) {
			// A different failure would mask the one being pinned, so name it.
			expect(false, string("expected std::logic_error, got: ") + e.what());
		}
		expect(threw, "sending with a=sframe negotiated and no SFrame packetizer was allowed, so "
		              "plaintext would go out on a track advertised as protected");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// SFrame protects media, not control traffic, so the refusal must not stop RTCP. outgoing() only
// types a message as Control when the track has no handler chain (a chain carries RTCP out through
// sendRtcpApp() instead), so that is the configuration this pins: a=sframe negotiated, nothing
// installed, RTCP still allowed through while media would be refused.
TestResult test_sframe_send_guard_exempts_rtcp() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0F0F0F0F, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		// Receiver Report: valid RTCP, so IsRtcp() promotes it to Control.
		try {
			track->send(makeRtcp(/*packetType=*/201, /*count=*/0, /*bodyWords=*/1, /*fill=*/0x00));
		} catch (const std::logic_error &e) {
			expect(false,
			       string("the guard refused RTCP, which SFrame does not protect: ") + e.what());
		} catch (const std::exception &) {
			// Expected: no transport.
		}

		// Same track, same absent chain: media is refused, which is what makes the line above mean
		// something rather than just reflecting a guard that never armed.
		//
		// The filler byte matters: IsRtcp() demultiplexes on payload type and treats 64-95 as RTCP,
		// so a byte in that range would have this frame promoted to Control on a track with no
		// handler -- exempted, and the assertion below would pass for the wrong reason.
		bool threw = false;
		try {
			track->sendFrame(binary(64, std::byte{0x01}), FrameInfo(0));
		} catch (const std::logic_error &) {
			threw = true;
		} catch (const std::exception &) {
		}
		expect(threw, "media was allowed out unprotected on a track advertising a=sframe");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// The counterfactual for the test above: the same track, with the send side installed after the
// receive side exactly as the deferred model does it, must not be refused. Without this, a guard
// that simply rejected every a=sframe track would pass the test above.
TestResult test_sframe_send_with_packetizer_allowed() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0F0F0F0F, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		track->useSFrame(std::make_shared<SessionKeyProvider>());

		auto rtpConfig = std::make_shared<RtpPacketizationConfig>(0x0F0F0F0F, kCname, kPayloadType,
		                                                          RtpPacketizer::VideoClockRate);
		track->chainMediaHandler(
		    std::make_shared<SFramePerFrameRtpPacketizer>(rtpConfig, makeSendProvider()));

		// The track has no transport, so sending cannot succeed -- but it must fail for that reason
		// rather than be refused by the guard.
		try {
			track->sendFrame(binary(64, std::byte{0x01}), FrameInfo(0));
		} catch (const std::logic_error &e) {
			expect(false,
			       string("the guard refused a correctly built deferred chain: ") + e.what());
		} catch (const std::exception &) {
			// Expected: no transport.
		}

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A receive chain installed at the PeerConnection rather than on the track. Both chains run on
// incoming media -- forwardMedia() runs the session-wide one and feeds what comes out to the
// track's -- so a session-wide SFrame depacketizer decrypts this m-line perfectly well, and the
// answer has to keep a=sframe.
//
// Asking only the track's chain got this wrong, and since a declined m-line is now stopped rather
// than downgraded, getting it wrong kills a working configuration outright instead of quietly
// weakening it. The positive control is the attribute surviving; the negative control is the
// sibling test below, where nothing anywhere applies SFrame and the m-line is stopped.
TestResult test_sframe_session_wide_receive_chain_keeps_attribute() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0E0E0E0E, kCname);
		offered.addSFrame();
		// Held for the duration: mTracks keeps only a weak_ptr, and a dropped track makes
		// populateLocalDescription rebuild the m-line from reciprocate(), bypassing the decision
		// under test entirely.
		auto localTrack = pc.addTrack(offered);

		PeerConnection peer(config);
		// Installed on the PeerConnection, not the track, and no useSFrame() anywhere -- so the
		// track's own chain is empty when the answer is built.
		peer.setMediaHandler(std::make_shared<SFramePerFrameVideoRtpDepacketizer>(
		    std::make_shared<SessionKeyProvider>()));
		shared_ptr<Track> peerTrack;
		peer.onTrack([&peerTrack](shared_ptr<Track> t) { peerTrack = std::move(t); });

		pc.setLocalDescription(Description::Type::Offer);
		auto offer = pc.localDescription();
		expect(offer.has_value(), "no offer was produced");
		peer.setRemoteDescription(*offer);
		auto answer = peer.localDescription();
		expect(answer.has_value(), "no answer was produced");

		expect(answer->hasSFrame(),
		       "the answerer stopped an m-line its session-wide chain would have decrypted: the "
		       "keep/stop decision is only asking the track's chain");
		// isOpen() is false here regardless -- no transport is ever established -- so the real
		// observable for "stopped" is isClosed().
		expect(peerTrack && !peerTrack->isClosed(),
		       "the answerer's track was closed even though its session-wide chain applies SFrame");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// The counterfactual for the test above: with no SFrame handler on the track OR the
// PeerConnection, the offered m-line really is one this side cannot honour, and it is stopped.
// Without this, the check above would pass just as well if the attribute were never removed at all.
TestResult test_sframe_no_receive_chain_anywhere_stops_mline() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0D0D0D0D, kCname);
		offered.addSFrame();
		// Held for the duration: mTracks keeps only a weak_ptr, and a dropped track makes
		// populateLocalDescription rebuild the m-line from reciprocate(), bypassing the decision
		// under test entirely.
		auto localTrack = pc.addTrack(offered);

		PeerConnection peer(config);
		shared_ptr<Track> peerTrack;
		peer.onTrack([&peerTrack](shared_ptr<Track> t) { peerTrack = std::move(t); });

		pc.setLocalDescription(Description::Type::Offer);
		auto offer = pc.localDescription();
		expect(offer.has_value(), "no offer was produced");
		peer.setRemoteDescription(*offer);
		auto answer = peer.localDescription();
		expect(answer.has_value(), "no answer was produced");

		expect(!answer->hasSFrame(),
		       "the answer kept a=sframe with nothing anywhere to decrypt it");
		expect(peerTrack && peerTrack->isClosed(),
		       "the m-line was not stopped even though nothing applies SFrame -- it was downgraded");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A session provider has to reach a track whose chain already holds the application's send-side
// packetizer. Both forms the example shows install that packetizer -- the deferred one after the
// answer, the upfront one before the offer -- so this is the ordinary case, not a corner.
//
// It regressed because the install was gated on MediaHandler::appliesSFrame(), which answers for
// the whole chain and is true of a packetizer. The receive side was then never installed, the
// answer still asserted a=sframe, and the peer encrypted into a track with nothing to decrypt: no
// warning, and hasSFrame() reporting protection that did not exist.
TestResult test_sframe_session_provider_with_packetizer_installed() {
	try {
		Configuration config;
		PeerConnection pc(config);
		pc.useSFrame(std::make_shared<SessionKeyProvider>());

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x0F0F0F0F, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		// The send side first, which is what defeated the old guard.
		auto rtpConfig = std::make_shared<RtpPacketizationConfig>(0x0F0F0F0F, kCname, kPayloadType,
		                                                          RtpPacketizer::VideoClockRate);
		auto packetizer =
		    std::make_shared<SFramePerFrameRtpPacketizer>(rtpConfig, makeSendProvider());
		track->chainMediaHandler(packetizer);

		// An answer confirming a=sframe is what triggers the session provider for a track this side
		// created, so drive a real exchange rather than poking the track directly.
		PeerConnection peer(config);
		// The peer needs a provider of its own, or it strips a=sframe from the answer and this end
		// never reaches the branch under test.
		peer.useSFrame(std::make_shared<SessionKeyProvider>());
		shared_ptr<Track> peerTrack;
		peer.onTrack([&peerTrack](shared_ptr<Track> t) { peerTrack = std::move(t); });

		pc.setLocalDescription(Description::Type::Offer);
		auto offer = pc.localDescription();
		expect(offer.has_value(), "no offer was produced");
		peer.setRemoteDescription(*offer);
		auto answer = peer.localDescription();
		expect(answer.has_value(), "no answer was produced");
		expect(answer->hasSFrame(), "the peer declined a=sframe, so this never exercised the path");
		pc.setRemoteDescription(*answer);

		// The receive side must now be installed alongside the packetizer, not instead of it.
		bool depacketizer = false, stillHasPacketizer = false;
		for (auto h = track->getMediaHandler(); h; h = h->next()) {
			if (dynamic_cast<SFramePerFrameVideoRtpDepacketizer *>(h.get()))
				depacketizer = true;
			if (h.get() == packetizer.get())
				stillHasPacketizer = true;
		}

		expect(
		    depacketizer,
		    "the session provider did not install a receive-side handler on a track that already "
		    "had a send-side packetizer, so a=sframe is negotiated with nothing to decrypt");
		expect(
		    stillHasPacketizer,
		    "installing the receive side dropped the application's packetizer, so this end would "
		    "stop encrypting");
		expect(track->description().hasSFrame(),
		       "a=sframe was dropped, so the test no longer covers the protected case");

		pc.close();
		peer.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// a=sframe is a property of the m-line: whatever RTP crosses it is SFrame encrypted, whichever way
// that RTP runs. Direction only decides which end packetizes and which depacketizes. Offers three
// m-lines at once -- sendonly, recvonly and sendrecv, all carrying a=sframe -- registers one
// session provider on the answerer, and checks that direction changes none of it: every m-line
// keeps the attribute in the answer and gets the receive-side handler. Nothing covered mixed
// directions before, and reciprocation inverting the direction is what makes it worth pinning.
TestResult test_sframe_multi_track_directions() {
	try {
		// A real offerer, so the offer carries valid ICE credentials and a fingerprint.
		Configuration offerConfig;
		PeerConnection offerer(offerConfig);

		auto makeDirectional = [](const string &mid, Description::Direction direction, SSRC ssrc) {
			Description::Video media(mid, direction);
			media.addH264Codec(kPayloadType);
			media.addSSRC(ssrc, kCname);
			media.addSFrame();
			return media;
		};
		// Held for the life of the test: PeerConnection keeps only weak references, so a discarded
		// track is destroyed at once and the offer comes out with no media at all.
		std::vector<shared_ptr<Track>> offeredTracks{
		    offerer.addTrack(makeDirectional("0", Description::Direction::SendOnly, 0x3001)),
		    offerer.addTrack(makeDirectional("1", Description::Direction::RecvOnly, 0x3002)),
		    offerer.addTrack(makeDirectional("2", Description::Direction::SendRecv, 0x3003))};

		Configuration config;
		PeerConnection pc(config);
		pc.useSFrame(std::make_shared<SessionKeyProvider>());

		std::map<string, shared_ptr<Track>> seen;
		pc.onTrack([&seen](shared_ptr<Track> track) { seen[track->mid()] = track; });

		std::promise<Description> offered;
		offerer.onLocalDescription([&offered](Description sdp) { offered.set_value(sdp); });
		offerer.setLocalDescription();
		auto offer = offered.get_future().get();
		pc.setRemoteDescription(offer);

		expect(seen.size() == 3, "expected three tracks, got " + to_string(seen.size()));
		for (const auto &mid : {"0", "1", "2"})
			expect(seen.count(mid) == 1, string("no track for mid=") + mid);

		// Reciprocation flips the direction, so the answerer's own view is the inverse of the
		// offer.
		expect(seen["0"]->direction() == Description::Direction::RecvOnly,
		       "mid=0 was offered sendonly, so the answerer should be recvonly");
		expect(seen["1"]->direction() == Description::Direction::SendOnly,
		       "mid=1 was offered recvonly, so the answerer should be sendonly");

		// a=sframe describes the m-line, not a direction: whatever RTP flows on it is SFrame
		// encrypted, and RTCP flows both ways regardless. So every m-line keeps the attribute in
		// the answer and gets the receive-side handler, whichever way its media happens to run. The
		// handler is inert on a send-only m-line -- an RtpDepacketizer only overrides incoming(),
		// and there is no inbound RTP -- and it keeps appliesSFrame() true so the attribute
		// survives. Encrypting what this end sends is the application's own packetizer.
		for (const auto &mid : {"0", "1", "2"}) {
			expect(seen[mid]->description().hasSFrame(),
			       string("mid=") + mid + " lost a=sframe in the answer");
			auto handler = seen[mid]->getMediaHandler();
			expect(handler && handler->appliesSFrame(),
			       string("mid=") + mid + " has no SFrame handler");
		}

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A send-only track carries a packetizer, not a depacketizer, and must still be able to keep
// "a=sframe": otherwise an answerer that only sends would silently transmit plaintext.
TestResult test_sframe_send_only_applies_sframe() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video sending("m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		                           "a=mid:0\r\n"
		                           "a=sendonly\r\n"
		                           "a=sframe\r\n"
		                           "a=rtpmap:96 H264/90000\r\n");
		auto track = pc.addTrack(sending);

		auto rtpConfig = std::make_shared<RtpPacketizationConfig>(0x2000, kCname, kPayloadType,
		                                                          RtpPacketizer::VideoClockRate);
		track->setMediaHandler(
		    std::make_shared<SFramePerFrameRtpPacketizer>(rtpConfig, makeSendProvider()));

		expect(track->getMediaHandler() && track->getMediaHandler()->appliesSFrame(),
		       "a send-only SFrame packetizer does not report applying SFrame, so the answer would "
		       "drop a=sframe and this end would send plaintext");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// One session-wide key provider registered with PeerConnection::useSFrame() before the offer
// is set, with nothing installed in the track callback. Covers the path the receiver example
// takes: both m-lines have to be set up by the library, video and audio alike, and the audio
// clock rate has to come from the negotiated description rather than from the application.
TestResult test_sframe_session_key_provider() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {},
		                               /*ratchetPeriod=*/0, /*ratchetStepBits=*/0,
		                               /*sharedKey=*/false, kFramesPerTrack,
		                               std::chrono::milliseconds(20), /*kidsSeen=*/nullptr,
		                               /*keepSFrameHandlerWhenDeclined=*/false,
		                               /*useSessionProvider=*/true, kGcm);
		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// An application may register a session provider unconditionally, without first checking
// Description::hasSFrame(). An m-line that never offered a=sframe must be unaffected: nothing
// installed, no attribute added to the answer, and plain media still arriving intact. The
// mixed case is the one that matters, since one registration covers both m-lines.
TestResult test_sframe_session_key_provider_without_offer() {
	try {
		auto outcomes = runOfferAnswer(
		    {{"0", Kind::Video, /*offerSFrame=*/false}, {"1", Kind::Video, /*offerSFrame=*/true}},
		    {}, /*ratchetPeriod=*/0, /*ratchetStepBits=*/0,
		    /*sharedKey=*/false, kFramesPerTrack, std::chrono::milliseconds(20),
		    /*kidsSeen=*/nullptr,
		    /*keepSFrameHandlerWhenDeclined=*/false,
		    /*useSessionProvider=*/true);
		// mid 0 never offered SFrame, so it must round-trip unprotected and unmangled.
		checkRoundTrip(outcomes, "0", false);

		// And the answer must not have asserted a=sframe for it. checkRoundTrip's own check reads
		// the offerer's track description, which says "unset" for this m-line no matter what came
		// back, so only the answer itself can catch the library adding the attribute to an m-line
		// that never asked. The harness separately asserts nothing was installed on that track.
		auto it = outcomes.find("0");
		expect(it != outcomes.end(), "no outcome recorded for mid 0");
		expect(!it->second.answerHasSFrame,
		       "the answer asserted a=sframe on an m-line that never offered it, so the session "
		       "provider was applied where it should have been left alone");

		// mid 1 did, and the same registration has to have covered it.
		checkRoundTrip(outcomes, "1", true);
		auto it1 = outcomes.find("1");
		expect(it1 != outcomes.end(), "no outcome recorded for mid 1");
		expect(it1->second.answerHasSFrame,
		       "the answer dropped a=sframe on the m-line that offered it, so the positive half of "
		       "this test is not exercising the session provider at all");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// An audio m-line may offer nothing but a static payload type (RFC 3551), which carries no
// a=rtpmap and so declares no RTP clock rate. Track::enableSFrame() cannot configure that m-line
// and throws for it, which is right -- but a session provider is documented as registerable
// unconditionally, so that throw must not escape setRemoteDescription() and take the whole
// negotiation with it. SFrame is declined for that one m-line and nothing else is affected, which
// is what the four checks below pin: the offer is processed, both tracks arrive, the video m-line
// offered alongside keeps SFrame, and audio is left plain rather than answering with an a=sframe
// nothing implements.
TestResult test_sframe_session_provider_unconfigurable_m_line() {
	try {
		// A real offerer, so the offer carries valid ICE credentials and a fingerprint.
		Configuration offerConfig;
		PeerConnection offerer(offerConfig);

		// Payload type 0 (PCMU) on its own, with no a=rtpmap: legal per RFC 3551/4566 and the
		// shape that leaves the clock rate underivable.
		Description::Media staticAudio("m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
		                               "a=mid:0\r\n"
		                               "a=sendrecv\r\n"
		                               "a=sframe\r\n");
		expect(staticAudio.hasSFrame(), "the audio m-line lost a=sframe");
		expect(!staticAudio.hasPayloadType(0),
		       "payload type 0 should carry no rtpmap, or there is no clock rate to fail on");

		Description::Video video("1", Description::Direction::SendOnly);
		video.addH264Codec(kPayloadType);
		video.addSSRC(0x4001, kCname);
		video.addSFrame();

		// Held for the life of the test: PeerConnection keeps only weak references, so a discarded
		// track is destroyed at once and the offer comes out with no media at all. Audio first, so
		// an escaping throw would abort the negotiation before the video m-line is reached.
		std::vector<shared_ptr<Track>> offeredTracks{offerer.addTrack(staticAudio),
		                                             offerer.addTrack(video)};

		Configuration config;
		PeerConnection pc(config);
		// Registered unconditionally, without first inspecting the offer, as the documentation on
		// PeerConnection::useSFrame() says an application may.
		pc.useSFrame(std::make_shared<SessionKeyProvider>());

		std::map<string, shared_ptr<Track>> seen;
		pc.onTrack([&seen](shared_ptr<Track> track) { seen[track->mid()] = track; });

		std::promise<Description> offered;
		offerer.onLocalDescription([&offered](Description sdp) { offered.set_value(sdp); });
		offerer.setLocalDescription();
		auto offer = offered.get_future().get();
		expect(offer.hasSFrame(), "the offer carries no a=sframe, so nothing here is under test");

		// The core of it: one m-line that cannot be configured must not take the negotiation down.
		try {
			pc.setRemoteDescription(offer);
		} catch (const std::exception &e) {
			expect(false, string("setRemoteDescription() threw, so an audio m-line offering only a "
			                     "static payload type aborted the whole negotiation: ") +
			                  e.what());
		}

		// An answer was still produced for the offer.
		auto answer = pc.localDescription();
		expect(answer.has_value() && answer->type() == Description::Type::Answer,
		       "no answer was generated for the offer");

		// And both m-lines were still surfaced to the application.
		expect(seen.size() == 2, "expected two tracks, got " + to_string(seen.size()));
		expect(seen.count("0") == 1, "no track for the audio m-line");
		expect(seen.count("1") == 1, "no track for the video m-line");

		// The video m-line is untouched by its neighbour: the session provider was applied to it
		// and a=sframe survives into the answer.
		auto videoHandler = seen["1"]->getMediaHandler();
		expect(videoHandler && videoHandler->appliesSFrame(),
		       "the video m-line has no SFrame handler, so one unconfigurable m-line damaged it");
		expect(seen["1"]->description().hasSFrame(),
		       "the video m-line lost a=sframe in the answer");

		// SFrame is declined on the audio m-line rather than half-enabled: nothing in its chain
		// applies SFrame, so the existing strip takes a=sframe out of the answer.
		auto audioHandler = seen["0"]->getMediaHandler();
		expect(!(audioHandler && audioHandler->appliesSFrame()),
		       "the audio m-line got an SFrame handler despite declaring no RTP clock rate");
		expect(
		    !seen["0"]->description().hasSFrame(),
		    "the audio m-line still asserts a=sframe in the answer, but nothing there implements "
		    "SFrame");

		pc.close();
		offerer.close();
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
// move and follow the per-SSRC key chain. Every frame must still round-trip, and the receiver
// must have been asked about exactly one key generation, since a ratchet moves the step and never
// the generation. That the sender ratcheted at all is pinned at the unit level.
TestResult test_sframe_multi_track_ratcheting() {
	try {
		const uint64_t ratchetPeriod = 5;  // seconds
		const uint8_t ratchetStepBits = 4; // steps 0..15 within the key generation
		const size_t frames = 26;
		const auto gap = std::chrono::milliseconds(300); // ~7.8s, comfortably past one ratchet

		std::map<string, std::set<uint64_t>> kidsSeen;
		auto outcomes = runOfferAnswer({{"0", Kind::Video}, {"1", Kind::Audio}}, {}, ratchetPeriod,
		                               ratchetStepBits, /*sharedKey=*/false, frames, gap, &kidsSeen,
		                               /*keepSFrameHandlerWhenDeclined=*/false,
		                               /*useSessionProvider=*/false, kGcm);

		checkRoundTrip(outcomes, "0", true);
		checkRoundTrip(outcomes, "1", true);

		// A provider is only ever asked about a key generation, never a ratchet step, so a
		// ratchet must not change what it is asked for. Seeing exactly one generation is
		// what proves the ratchet stayed inside its field: if it had escaped, the receiver
		// would have asked for a different generation and got nullopt, and the round-trip
		// checks above would already have failed.
		//
		// That the sender actually ratcheted, and the receiver followed the chain, is pinned at the
		// unit level by testLiveRatchetWrapsAroundTheField, which can see the KID directly. Here it shows up as frames continuing to decode across the period.
		for (const string &mid : {"0", "1"}) {
			auto it = kidsSeen.find(mid);
			expect(it != kidsSeen.end(), "mid=" + mid + ": no key generation recorded");
			expect(it->second.size() == 1,
			       "mid=" + mid + ": the receiver was asked about " + to_string(it->second.size()) +
			           " key generations across " + to_string(frames) +
			           " frames, so a ratchet escaped its field in the KID");
		}

		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A mixed offer, which is legal and has to keep working: one m-line that never asked for SFrame,
// one that asked and got it, and one that asked and was refused. Those are now the only three
// outcomes an m-line can have, and this is the only test that puts all three in one session.
//
// The point is that they are independent. Stopping mid 2 must not disturb mid 1's protection or
// mid 0's plaintext, and the plaintext m-line must not be dragged into SFrame by a sibling -- both
// of which are easy to get wrong, since the decision is per m-line but the transport is shared.
// Kinds are mixed too, so the outcome cannot be keyed off audio vs video.
TestResult test_sframe_mixed_offer_protected_plain_and_stopped() {
	try {
		auto outcomes = runOfferAnswer({{"0", Kind::Audio, /*offerSFrame=*/false},
		                                {"1", Kind::Video, /*offerSFrame=*/true},
		                                {"2", Kind::Video, /*offerSFrame=*/true}},
		                               {"2"});

		// Never offered: an ordinary track, delivered intact and in the clear.
		checkRoundTrip(outcomes, "0", false);
		checkWireProtection(outcomes, "0", /*expectProtected=*/false);

		// Offered and kept: protected, and the ciphertext check has mid 0 above as its control.
		checkRoundTrip(outcomes, "1", true);
		checkWireProtection(outcomes, "1", /*expectProtected=*/true);

		// Offered and refused: stopped rather than downgraded.
		checkStopped(outcomes, "2");

		// RTCP is not SFrame's to protect and must survive on both live m-lines.
		checkRtcpPassedThrough(outcomes, "0");
		checkRtcpPassedThrough(outcomes, "1");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// The interesting case: the answer declines SFrame on one audio and one video m-line and keeps it
// on the others. The two that kept it must round-trip protected; the two that lost it must be
// stopped rather than downgraded. That only holds if the decision is made per m-line -- a session
// that failed as a whole, or one that downgraded the pair, would both show up here -- and if it is
// independent of media type.
TestResult test_sframe_multi_track_partial_answer() {
	try {
		auto outcomes = runOfferAnswer(
		    {{"0", Kind::Video}, {"1", Kind::Audio}, {"2", Kind::Video}, {"3", Kind::Audio}},
		    {"1", "2"}, /*ratchetPeriod=*/0, /*ratchetStepBits=*/0, /*sharedKey=*/false,
		    kFramesPerTrack, std::chrono::milliseconds(20), /*kidsSeen=*/nullptr,
		    /*keepSFrameHandlerWhenDeclined=*/false, /*useSessionProvider=*/false, kAes128Ctr);
		checkRoundTrip(outcomes, "0", true);
		checkStopped(outcomes, "1");
		checkStopped(outcomes, "2");
		checkRoundTrip(outcomes, "3", true);
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A multi-codec offer with the send chain deferred until the answer has been applied. This is the
// only structure that works once an offer lists more than one codec: the answerer chooses, and the
// offerer cannot know which payload type to packetize for until it reads the answer. H.264 is
// offered first and AV1 last, and the answerer narrows to VP8 in the middle, so an offerer that
// assumed the offer's own first choice -- or consulted its own track description, which the library
// never narrows -- would packetize for a codec the answer had removed. Narrowing the answer is the
// answerer's own work here, since libdatachannel echoes back every codec it was offered.
TestResult test_sframe_deferred_chain_multi_codec() {
	try {
		auto outcomes = runOfferAnswer(
		    {{"0", Kind::Video}}, {}, /*ratchetPeriod=*/0, /*ratchetStepBits=*/0,
		    /*sharedKey=*/false, kFramesPerTrack, std::chrono::milliseconds(20),
		    /*kidsSeen=*/nullptr, /*keepSFrameHandlerWhenDeclined=*/false,
		    /*useSessionProvider=*/false, kAes256Ctr,
		    ChainPlan{/*multipleVideoCodecs=*/true, /*narrowAnswerTo=*/kVp8PayloadType,
		              /*deferSendChain=*/true});
		checkRoundTrip(outcomes, "0", true);
		checkNegotiatedCodec(outcomes, "0", kVp8PayloadType);
		checkRtcpPassedThrough(outcomes, "0");
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A session provider registered with PeerConnection::useSFrame() has to cover a track this side
// created, not only m-lines the peer introduced. Otherwise an offerer negotiates a=sframe and has
// nothing to decrypt with: no handler, no warning, and hasSFrame() still true, so its inbound media
// is silently lost. Every other useSFrame() test here is on the answerer, so this pins the offerer.
TestResult test_sframe_session_provider_on_offerer() {
	try {
		Configuration offerConfig;
		PeerConnection offerer(offerConfig);

		Configuration answerConfig;
		PeerConnection answerer(answerConfig);

		offerer.onLocalDescription(
		    [&answerer](Description sdp) { answerer.setRemoteDescription(string(sdp)); });
		answerer.onLocalDescription(
		    [&offerer](Description sdp) { offerer.setRemoteDescription(string(sdp)); });

		// Registered before any description is exchanged, as the documentation says to.
		offerer.useSFrame(std::make_shared<SessionKeyProvider>());
		// The answerer needs one too, or it would decline and strip the attribute from the answer.
		answerer.useSFrame(std::make_shared<SessionKeyProvider>());

		Description::Video media("0", Description::Direction::SendRecv);
		media.addH264Codec(kPayloadType);
		media.addSSRC(0x6001, kCname);
		media.addSFrame();

		// Held for the life of the test: PeerConnection keeps only a weak reference.
		auto track = offerer.addTrack(media);
		expect(track->description().hasSFrame(), "the offered m-line lost a=sframe");

		offerer.setLocalDescription();

		// The answer is applied inside the callback above, so the enable has run by the time the
		// offerer has a remote description.
		int attempts = 20;
		while (attempts-- && !offerer.remoteDescription().has_value())
			this_thread::sleep_for(100ms);
		expect(offerer.remoteDescription().has_value(), "the offerer never received an answer");

		expect(track->description().hasSFrame(),
		       "the answer declined a=sframe, so this is not testing the enable path");

		auto handler = track->getMediaHandler();
		expect(handler && handler->appliesSFrame(),
		       "the offerer's own track got no SFrame handler from the session provider, so it "
		       "negotiated a=sframe with nothing to decrypt with");

		offerer.close();
		answerer.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// The clockRate argument to Track::useSFrame() is what the audio depacketizer runs with, whatever
// the m-line declares. On an m-line mixing codecs of different rates only the application knows
// which rate its frames are timed against, so an explicit value has to win over anything derivable
// from the SDP -- and it has to make an m-line that declares no rate at all configurable, since the
// derivation has nothing to work from there and refuses the track.
TestResult test_sframe_track_explicit_clock_rate() {
	try {
		Configuration config;
		PeerConnection pc(config);

		// a=rtpmap declares 48000 and the application says 8000, so a rate taken from the wrong
		// place cannot come out looking right.
		Description::Media declared("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
		                            "a=mid:0\r\n"
		                            "a=sendrecv\r\n"
		                            "a=sframe\r\n"
		                            "a=rtpmap:111 opus/48000/2\r\n");
		auto declaredTrack = pc.addTrack(declared);
		declaredTrack->useSFrame(std::make_shared<SessionKeyProvider>(), 8000);

		const uint32_t declaredRate = observedClockRate(declaredTrack, 0x7001, 111);
		expect(declaredRate == 8000,
		       "the depacketizer runs at " + to_string(declaredRate) +
		           " Hz but useSFrame() was given 8000, so the m-line's own a=rtpmap overrode the "
		           "application and every frame would be timed against a rate it did not choose");

		// Payload type 0 (PCMU) alone carries no a=rtpmap (RFC 4566), so nothing is derivable here
		// and the explicit rate is the only thing that can configure this m-line.
		Description::Media staticOnly("m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
		                              "a=mid:1\r\n"
		                              "a=sendrecv\r\n"
		                              "a=sframe\r\n");
		expect(!staticOnly.hasPayloadType(0),
		       "payload type 0 should carry no rtpmap, or there is no underivable rate to rescue");
		auto staticTrack = pc.addTrack(staticOnly);
		staticTrack->useSFrame(std::make_shared<SessionKeyProvider>(), 16000);

		const uint32_t staticRate = observedClockRate(staticTrack, 0x7002, 0);
		expect(staticRate == 16000,
		       "the depacketizer runs at " + to_string(staticRate) +
		           " Hz but useSFrame() was given 16000 for an m-line that declares no rate");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// With no explicit rate the audio depacketizer takes the first negotiated payload type that carries
// an a=rtpmap. A static payload type declares none (RFC 4566), so the search has to step past it
// rather than stop there, and nothing may fall back to a default rate: a rate that is merely
// plausible mis-times every frame the track delivers, silently.
TestResult test_sframe_track_clock_rate_from_rtpmap() {
	try {
		Configuration config;
		PeerConnection pc(config);

		// Payload type 8 (PCMA) first with no a=rtpmap, then a mapped format at a rate that is
		// nobody's default, so a derivation that stopped at the static type or fell back to 48000
		// reports the wrong number rather than accidentally the right one.
		Description::Media mixed("m=audio 9 UDP/TLS/RTP/SAVPF 8 111\r\n"
		                         "a=mid:0\r\n"
		                         "a=sendrecv\r\n"
		                         "a=sframe\r\n"
		                         "a=rtpmap:111 L16/16000\r\n");
		expect(!mixed.hasPayloadType(8),
		       "payload type 8 should carry no rtpmap, or the derivation has nothing to step past");
		expect(mixed.hasPayloadType(111), "the m-line lost the mapped payload type");

		auto track = pc.addTrack(mixed);
		// No rate passed: the negotiated description is the only source left.
		track->useSFrame(std::make_shared<SessionKeyProvider>());

		const uint32_t rate = observedClockRate(track, 0x7101, 111);
		expect(rate == 16000, "the depacketizer runs at " + to_string(rate) +
		                          " Hz, but the only a=rtpmap on the m-line declares 16000");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// Both entry points refuse a null key provider at the call, rather than installing a handler that
// would dereference it on the first frame. Track::useSFrame() must also leave the chain untouched
// when it refuses: a handler installed alongside the throw would keep a=sframe in the answer with
// no key material behind it.
TestResult test_sframe_null_key_provider_refused() {
	try {
		Configuration config;
		PeerConnection pc(config);

		Description::Video offered(kCname, Description::Direction::SendRecv);
		offered.addH264Codec(kPayloadType);
		offered.addSSRC(0x7301, kCname);
		offered.addSFrame();
		auto track = pc.addTrack(offered);

		bool trackThrew = false;
		try {
			track->useSFrame(nullptr);
		} catch (const std::invalid_argument &) {
			trackThrew = true;
		} catch (const std::exception &e) {
			// A different failure would mask the one being pinned, so name it.
			expect(false,
			       string("Track::useSFrame(nullptr): expected std::invalid_argument, got: ") +
			           e.what());
		}
		expect(trackThrew, "Track::useSFrame() accepted a null key provider, so the first frame to "
		                   "arrive would be decrypted against nothing");
		expect(
		    track->getMediaHandler() == nullptr,
		    "Track::useSFrame() installed a handler while refusing the call, so the answer would "
		    "assert a=sframe with no key material behind it");

		bool pcThrew = false;
		try {
			pc.useSFrame(nullptr);
		} catch (const std::invalid_argument &) {
			pcThrew = true;
		} catch (const std::exception &e) {
			expect(
			    false,
			    string(
			        "PeerConnection::useSFrame(nullptr): expected std::invalid_argument, got: ") +
			        e.what());
		}
		expect(pcThrew, "PeerConnection::useSFrame() accepted a null key provider, so every m-line "
		                "negotiating a=sframe in this session would decrypt against nothing");

		pc.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

// A session provider is applied before the track callback runs, which is what makes the callback's
// documented escape possible: swapping in different key material with Track::useSFrame(). Both
// halves are pinned here -- the handler is already in the chain when onTrack fires, and the
// provider the chain ends up consulting is the callback's rather than the session's.
//
// The override holds a key generation the session provider does not, so a frame under it decrypts
// only if the swap really took effect. Comparing handler pointers would not do: it cannot tell a
// replacement apart from a second SFrame depacketizer stacked in front of the first, which would
// leave the session provider's handler still reading frames.
TestResult test_sframe_session_provider_before_track_callback() {
	try {
		const uint64_t overrideKid = 7;
		const SSRC ssrc = 0x7201;

		// A real offerer, so the offer carries valid ICE credentials and a fingerprint.
		Configuration offerConfig;
		PeerConnection offerer(offerConfig);

		Description::Video media("0", Description::Direction::SendOnly);
		media.addH264Codec(kPayloadType);
		media.addSSRC(ssrc, kCname);
		media.addSFrame();
		// Held for the life of the test: PeerConnection keeps only a weak reference, so a discarded
		// track is destroyed at once and the offer comes out with no media at all.
		auto offeredTrack = offerer.addTrack(media);

		auto sessionProvider = std::make_shared<SessionKeyProvider>();

		// Same suite and KID layout -- those belong to the session, not to the key -- but holding a
		// generation the session provider never registered.
		auto overrideProvider = std::make_shared<SessionKeyProvider>();
		overrideProvider->addKey(overrideKid,
		                         SFrameReceiveKey{sessionKey(baseKeySize(kAes256Ctr.video))});

		Configuration config;
		PeerConnection pc(config);
		pc.useSFrame(sessionProvider);

		shared_ptr<Track> seen;
		bool installedBeforeCallback = false;
		string callbackError;
		pc.onTrack([&](shared_ptr<Track> track) {
			// Read before anything is changed: if the session provider were applied after this
			// callback, overriding it here would be the only way to get SFrame at all rather than a
			// way to change what was already set up.
			auto installed = track->getMediaHandler();
			installedBeforeCallback = installed && installed->appliesSFrame();
			try {
				track->useSFrame(overrideProvider);
			} catch (const std::exception &e) {
				callbackError = e.what(); // an exception here would be swallowed by the callback
			}
			seen = track;
		});

		std::promise<Description> offered;
		offerer.onLocalDescription([&offered](Description sdp) { offered.set_value(sdp); });
		offerer.setLocalDescription();
		auto offer = offered.get_future().get();
		pc.setRemoteDescription(offer);

		expect(callbackError.empty(), "the override threw: " + callbackError);
		expect(seen != nullptr, "onTrack never fired, so there was nothing to override");
		expect(installedBeforeCallback,
		       "no SFrame handler was in the chain when onTrack fired, so the session provider is "
		       "applied after the callback and a callback cannot override it");

		// Overriding is not declining: the answer still asserts a=sframe.
		expect(seen->description().hasSFrame(),
		       "a=sframe was dropped from the answer after the callback overrode the provider");

		// Exactly one handler decrypts. Two stacked SFrame depacketizers would both try, and the
		// one behind would be handed a frame the one in front had already unwrapped.
		int sframeHandlers = 0;
		for (auto h = seen->getMediaHandler(); h; h = h->next())
			if (dynamic_cast<SFramePerFrameVideoRtpDepacketizer *>(h.get()))
				++sframeHandlers;
		expect(sframeHandlers == 1,
		       "the chain holds " + to_string(sframeHandlers) +
		           " SFrame depacketizers: the override has to replace the session provider's, not "
		           "stack in front of it");

		// A frame under the override's own key generation, which only its provider can answer for.
		auto sendProvider = std::make_shared<SFrameSendKeyProvider>(
		    kAes256Ctr.video, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0,
		    /*perSsrcDerivation=*/true,
		    SFrameSendKey{sessionKey(baseKeySize(kAes256Ctr.video)), overrideKid});
		auto rtpConfig = std::make_shared<RtpPacketizationConfig>(ssrc, kCname, kPayloadType,
		                                                          RtpPacketizer::VideoClockRate);
		SFramePerFrameRtpPacketizer packetizer(rtpConfig, sendProvider);

		auto frame = makeFrame(400, 0x7200);
		auto frameInfo = std::make_shared<FrameInfo>(9000u);
		frameInfo->payloadType = kPayloadType;
		message_vector packets{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(packets, [](message_ptr) {});

		auto head = seen->getMediaHandler();
		binary decoded;
		for (auto &packet : packets) {
			message_vector one{packet};
			head->incomingChain(one, [](message_ptr) {});
			for (auto &out : one)
				if (out && out->type != Message::Control && out->frameInfo)
					decoded.assign(out->begin(), out->end());
		}
		expect(decoded.size() == frame.size() &&
		           std::equal(frame.begin(), frame.end(), decoded.begin()),
		       "the frame did not decrypt under the callback's provider, so the session provider's "
		       "key material was still the one in use");
		expect(sessionProvider->requested().empty(),
		       "the session provider was asked for a key, so its handler was still reading frames "
		       "after the callback replaced it");

		pc.close();
		offerer.close();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

#endif // RTC_ENABLE_MEDIA
