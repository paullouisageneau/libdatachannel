/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "impl/sframeutility.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

namespace {

static const SSRC TEST_SSRC = 0x1234ABCD;
static const uint8_t TEST_PT = 96;
static const char *TEST_CNAME = "sframe-test";

// Independent literals so the static_asserts below fail if the on-wire descriptor bits in
// rtc/sframe.hpp change.
static constexpr uint8_t kSFrameDescriptorS = 0x80; // start
static constexpr uint8_t kSFrameDescriptorE = 0x40; // end
static constexpr uint8_t kSFrameDescriptorT = 0x20; // payload origin (0 raw, 1 packetized)
static_assert(SFRAME_DESCRIPTOR_S == kSFrameDescriptorS, "SFrame descriptor S bit changed");
static_assert(SFRAME_DESCRIPTOR_E == kSFrameDescriptorE, "SFrame descriptor E bit changed");
static_assert(SFRAME_DESCRIPTOR_T == kSFrameDescriptorT, "SFrame descriptor T value changed");

// RFC 9605-style key provider: it is provisioned only with the *session* (master) key
// and hands it back for any KID. The per-SSRC derivation belongs to the library, on both
// the send and the receive side, so the provider never sees an SSRC and keeps no
// per-call state -- which is what makes it safe to share across tracks.
// It also records every KID it was asked for, so tests can assert what the decoder
// looked up and how often.
class SessionKeyProvider final : public SFrameKeyProvider {
public:
	bool usePerSSRCDerivation() const override { return mPerSSRC; }
	SessionKeyProvider(uint8_t cipherSuiteId, binary sessionKey, bool perSSRC = true)
	    : mCipherSuiteId(cipherSuiteId), mSessionKey(std::move(sessionKey)), mPerSSRC(perSSRC) {}

	std::optional<SFrameKeyDetails> getKeyDetails(uint64_t kid) override {
		mLookups++;
		return SFrameKeyDetails{mCipherSuiteId, mSessionKey, /*ctrStart=*/0, SFrameKeyUse::Decrypt};
	}

	size_t lookups() const { return mLookups; }

private:
	const uint8_t mCipherSuiteId;
	const binary mSessionKey;
	const bool mPerSSRC;
	std::atomic<size_t> mLookups{0};
};

// Like SessionKeyProvider but the master key can be swapped at runtime, modelling a
// provider that re-keys a KID in place.
class MutableKeyProvider final : public SFrameKeyProvider {
public:
	bool usePerSSRCDerivation() const override { return true; }
	MutableKeyProvider(uint8_t cipherSuiteId, binary sessionKey)
	    : mCipherSuiteId(cipherSuiteId), mSessionKey(std::move(sessionKey)) {}

	void setKey(binary sessionKey) { mSessionKey = std::move(sessionKey); }

	std::optional<SFrameKeyDetails> getKeyDetails(uint64_t kid) override {
		return SFrameKeyDetails{mCipherSuiteId, mSessionKey, /*ctrStart=*/0, SFrameKeyUse::Decrypt};
	}

private:
	const uint8_t mCipherSuiteId;
	binary mSessionKey;
};

// Build a deterministic plaintext frame of the given size.
binary makeFrame(size_t size, uint8_t seed) {
	binary frame;
	frame.reserve(size);
	for (size_t i = 0; i < size; ++i)
		frame.push_back(static_cast<std::byte>((seed + i) & 0xFF));
	return frame;
}

// A session key of the size expected by the given cipher suite.
binary makeSessionKey(uint8_t cipherSuiteId) {
	// Suites 0x01-0x04 use AES-128 (16-byte keys); 0x05+ use AES-256 (32-byte).
	size_t len = (cipherSuiteId >= 0x05) ? 32 : 16;
	binary key(len);
	for (size_t i = 0; i < len; ++i)
		key[i] = std::byte(0x40 + i);
	return key;
}

shared_ptr<RtpPacketizationConfig> makeConfig(uint32_t clockRate) {
	auto config = std::make_shared<RtpPacketizationConfig>(TEST_SSRC, TEST_CNAME, TEST_PT,
	                                                       clockRate);
	return config;
}

// Run the SFrame packetizer over a set of plaintext frames and collect the
// resulting RTP packets. Each frame gets a distinct RTP timestamp.
message_vector packetizeFramesWithSsrc(uint32_t clockRate, SSRC ssrc,
                                       const SFrameConfig &sframeConfig,
                                       const std::vector<binary> &frames) {
	auto config = std::make_shared<RtpPacketizationConfig>(ssrc, TEST_CNAME, TEST_PT, clockRate);
	SFrameRtpPacketizer packetizer(config, sframeConfig, SFrameMode::PerFrame);

	message_vector messages;
	uint32_t timestamp = 1000;
	for (const auto &frame : frames) {
		auto frameInfo = std::make_shared<FrameInfo>(timestamp);
		frameInfo->payloadType = TEST_PT;
		messages.push_back(make_message(binary(frame), frameInfo));
		timestamp += 3000;
	}

	// outgoing() encrypts, fragments, and swaps the produced RTP packets back
	// into `messages`. The send callback is unused by RtpPacketizer.
	packetizer.outgoing(messages, [](message_ptr) {});
	return messages;
}

message_vector packetizeFrames(uint32_t clockRate, const SFrameConfig &sframeConfig,
                               const std::vector<binary> &frames) {
	return packetizeFramesWithSsrc(clockRate, TEST_SSRC, sframeConfig, frames);
}

// Rewrite a packet's RTP timestamp, sequence number and marker bit. Used to build
// per-packet SFrame streams, where several independent SFrame objects share one RTP
// timestamp and only the last packet of the frame carries the marker.
message_ptr withRtpFraming(const message_ptr &packet, uint32_t timestamp, uint16_t seq,
                           bool marker) {
	auto copy = make_message(binary(packet->begin(), packet->end()), Message::Binary);
	auto header = reinterpret_cast<RtpHeader *>(copy->data());
	header->setTimestamp(timestamp);
	header->setSeqNumber(seq);
	header->setMarker(marker);
	return copy;
}

// Return the 1-byte SFrame RFC S/E/T descriptor from an RTP packet — the byte
// immediately after the RTP header (and any CSRC/extension).
uint8_t descriptorByte(const message_ptr &packet) {
	auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
	auto hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
	return static_cast<uint8_t>((*packet)[hdrSize]);
}

// Overwrite the 1-byte SFrame RFC descriptor of an RTP packet.
void setDescriptorByte(const message_ptr &packet, uint8_t value) {
	auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
	auto hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
	(*packet)[hdrSize] = static_cast<std::byte>(value);
}

// Rewrite an RTP packet to insert `count` CSRC identifiers right after the
// fixed 12-byte header (updating the CC field), shifting everything after it.
// The packetizer never emits CSRCs, so this is built by hand to exercise
// CSRC-aware header-size accounting. Assumes the input packet has no CSRCs.
message_ptr withCsrcs(const message_ptr &packet, uint8_t count) {
	const auto &in = *packet;
	binary out;
	out.reserve(in.size() + count * 4);
	// Fixed 12-byte RTP header.
	out.insert(out.end(), in.begin(), in.begin() + sizeof(RtpHeader));
	// Set the CC nibble (low 4 bits of the first byte).
	out[0] = static_cast<std::byte>((static_cast<uint8_t>(out[0]) & 0xF0) | (count & 0x0F));
	// Insert count * 4 bytes of (arbitrary) CSRC identifiers.
	out.insert(out.end(), static_cast<size_t>(count) * 4, std::byte{0xAB});
	// Everything after the fixed header (descriptor + payload).
	out.insert(out.end(), in.begin() + sizeof(RtpHeader), in.end());
	return make_message(std::move(out), Message::Binary);
}

// Append `pad` bytes of RTP padding to a packet and set the P bit. The last
// byte holds the total padding count (including itself), per RFC 3550 section 5.1.
// `pad` must be >= 1.
message_ptr withPadding(const message_ptr &packet, uint8_t pad) {
	const auto &in = *packet;
	binary out(in.begin(), in.end());
	// Set the P (padding) bit — bit 5 of the first octet.
	out[0] = static_cast<std::byte>(static_cast<uint8_t>(out[0]) | 0x20);
	out.insert(out.end(), pad, std::byte{0x00});
	out.back() = static_cast<std::byte>(pad);
	return make_message(std::move(out), Message::Binary);
}

message_vector depacketizeVideo(message_vector packets, shared_ptr<SFrameKeyProvider> provider) {
	SFrameVideoRtpDepacketizer depacketizer(std::move(provider));
	depacketizer.incoming(packets, [](message_ptr) {});
	return packets;
}

// One packet per incoming() call, which is what Track::incoming does on the wire. The batch
// form above lets several groups exist at once and so hides anything that only goes wrong
// when the end-of-batch flush runs after every packet.
message_vector depacketizeVideoPerPacket(message_vector packets,
                                         shared_ptr<SFrameKeyProvider> provider) {
	SFrameVideoRtpDepacketizer depacketizer(std::move(provider));
	message_vector decoded;
	for (auto &packet : packets) {
		message_vector one{std::move(packet)};
		depacketizer.incoming(one, [](message_ptr) {});
		for (auto &out : one)
			decoded.push_back(std::move(out));
	}
	return decoded;
}

message_vector depacketizeAudio(message_vector packets, uint32_t clockRate,
                                shared_ptr<SFrameKeyProvider> provider) {
	SFrameAudioRtpDepacketizer depacketizer(clockRate, std::move(provider));
	depacketizer.incoming(packets, [](message_ptr) {});
	return packets;
}

bool sameBytes(const binary &expected, const message_ptr &actual) {
	if (!actual || actual->size() != expected.size())
		return false;
	return std::equal(expected.begin(), expected.end(), actual->begin());
}

void check(bool condition, const std::string &message) {
	if (!condition)
		throw std::runtime_error(message);
}

// Verify SFrame frame-boundary marking for a single frame: the last RTP packet
// carries the marker bit and every earlier packet does not. Works for both a
// single-packet frame (the one packet is the last, so its marker is set) and a
// fragmented multi-packet frame.
void checkFrameMarkers(const message_vector &packets) {
	check(!packets.empty(), "expected at least one RTP packet");
	for (size_t i = 0; i < packets.size(); ++i) {
		auto header = reinterpret_cast<const RtpHeader *>(packets[i]->data());
		const bool marker = header->marker() != 0;
		const bool isLast = (i + 1 == packets.size());
		check(marker == isLast, "packet " + std::to_string(i) + ": marker=" +
		                            std::to_string(marker) + " isLast=" + std::to_string(isLast));
	}
}

SFrameConfig makeSFrameConfig(uint8_t cipherSuiteId, const binary &sessionKey) {
	SFrameConfig config;
	config.keyDetails = SFrameKeyDetails{cipherSuiteId, sessionKey, /*ctrStart=*/0,
	                                     SFrameKeyUse::Encrypt};
	config.ratchetPeriod = 0; // never ratchet: kid stays constant for the test
	config.perSsrcDerivation = true;
	return config;
}

// --- Individual scenarios -------------------------------------------------

// RFC 9605 Section 4.4.1 requires a base key be marked for one direction. An unmarked key, or
// one marked for the other direction, is refused rather than quietly serving both.
void testKeyUseMustMatchDirection() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	auto rtpConfig = std::make_shared<RtpPacketizationConfig>(
	    TEST_SSRC, "test", 96, RtpPacketizer::VideoClockRate);

	auto throwsWith = [&](std::optional<SFrameKeyUse> use) {
		auto config = makeSFrameConfig(suite, sessionKey);
		config.keyDetails.use = use;
		try {
			SFrameRtpPacketizer packetizer(rtpConfig, config, SFrameMode::PerFrame);
		} catch (const std::invalid_argument &) {
			return true;
		}
		return false;
	};

	check(throwsWith(std::nullopt), "an unmarked base key was accepted for sending");
	check(throwsWith(SFrameKeyUse::Decrypt), "a decrypt-only base key was accepted for sending");

	// SFrameEncoder enforces it too, for callers not going through the packetizer.
	auto encoderConfig = makeSFrameConfig(suite, sessionKey);
	encoderConfig.keyDetails.use = SFrameKeyUse::Decrypt;
	bool encoderThrew = false;
	try {
		SFrameEncoder encoder(encoderConfig);
	} catch (const std::invalid_argument &) {
		encoderThrew = true;
	}
	check(encoderThrew, "SFrameEncoder accepted a decrypt-only base key");

	// And a provider handing back the sending key is refused on the receive side.
	auto frame = makeFrame(200, 0x4D);
	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frame});

	class EncryptMarkedProvider final : public SFrameKeyProvider {
	public:
		EncryptMarkedProvider(uint8_t suite, binary key) : mSuite(suite), mKey(std::move(key)) {}
		bool usePerSSRCDerivation() const override { return true; }
		std::optional<SFrameKeyDetails> getKeyDetails(uint64_t) override {
			return SFrameKeyDetails{mSuite, mKey, 0, SFrameKeyUse::Encrypt};
		}

	private:
		const uint8_t mSuite;
		const binary mKey;
	};

	auto wrongWay = std::make_shared<EncryptMarkedProvider>(suite, sessionKey);
	check(depacketizeVideo(packets, wrongWay).empty(),
	      "the decoder accepted a key its provider marked for encryption");
}

// perSsrcDerivation is not negotiated, so leaving it unset is a configuration error the
// application has to see rather than a default the library picks for it.
void testPerSsrcDerivationMustBeSet() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	SFrameConfig config;
	config.keyDetails = SFrameKeyDetails{suite, sessionKey, /*ctrStart=*/0,
	                                     SFrameKeyUse::Encrypt};
	config.keyGeneration = 1;
	// perSsrcDerivation deliberately left unset

	auto rtpConfig = std::make_shared<RtpPacketizationConfig>(
	    TEST_SSRC, "test", 96, RtpPacketizer::VideoClockRate);

	bool threw = false;
	try {
		SFrameRtpPacketizer packetizer(rtpConfig, config, SFrameMode::PerFrame);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	check(threw, "an SFrameConfig with no perSsrcDerivation was accepted");
}

// With derivation off the base key is used as-is, so a receiver whose provider also answers
// false decodes -- and one that answers true does not.
void testPerSsrcDerivationOffRoundTrips() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x3C);

	auto config = makeSFrameConfig(suite, sessionKey);
	config.perSsrcDerivation = false;

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate, config, {frame});

	auto matching = std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/false);
	auto decoded = depacketizeVideo(packets, matching);
	check(decoded.size() == 1 && sameBytes(frame, decoded.front()),
	      "a frame sent without per-SSRC derivation did not round-trip to a provider that "
	      "also skips it");

	auto mismatched = std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/true);
	check(depacketizeVideo(packets, mismatched).empty(),
	      "a receiver deriving per-SSRC decoded a frame sent without derivation");
}

// A peer choosing an RTP timestamp far in the future must not be able to park a group at the
// head of the reassembly buffer. Ordering the buffer by timestamp would do exactly that: the
// poison group compares as newest against every real frame and nothing evicts it, so no
// multi-packet frame is ever delivered again.
void testFarFutureTimestampDoesNotWedgeVideo() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(3000, 0x5B);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frame});
	check(packets.size() > 1, "expected a fragmented frame for this test");

	auto firstHeader = reinterpret_cast<const RtpHeader *>(packets.front()->data());
	const uint32_t timestamp = firstHeader->timestamp();
	const uint16_t seq = firstHeader->seqNumber();

	// Opens a run that never closes, so the group can never resolve on its own.
	auto poison = withRtpFraming(packets.front(), timestamp + 0x40000000u,
	                             static_cast<uint16_t>(seq + 1000), /*marker=*/false);
	setDescriptorByte(poison, SFRAME_DESCRIPTOR_S);

	message_vector input;
	input.push_back(std::move(poison));
	for (auto &packet : packets)
		input.push_back(std::move(packet));

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideoPerPacket(std::move(input), provider);
	check(decoded.size() == 1, "expected the real frame to survive a far-future timestamp, got " +
	                               std::to_string(decoded.size()) + " frames");
	check(sameBytes(frame, decoded.front()), "the frame did not round-trip");
}

// Video, single RTP packet: a small frame that fits one packet (marker bit
// set on that one packet, no fragmentation).
void testSmallFrameRoundTrip() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x11);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frame});
	check(packets.size() == 1, "small frame should produce exactly one RTP packet, got " +
	                               std::to_string(packets.size()));
	checkFrameMarkers(packets);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.size() == 1, "expected 1 decrypted frame, got " + std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "small frame did not round-trip");
}

// Video, multiple RTP packets: a large frame spanning several packets,
// exercising SFrame RFC fragmentation on the send side and reassembly on the
// receive side (marker bit set only on the last packet).
void testLargeFrameRoundTrip() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(5000, 0x22);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frame});
	check(packets.size() > 1, "large frame should fragment into multiple RTP packets, got " +
	                              std::to_string(packets.size()));
	checkFrameMarkers(packets);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.size() == 1, "expected 1 decrypted frame, got " + std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "large fragmented frame did not round-trip");
}

// Several sequential frames: the counter must advance and every frame must
// decrypt back to its original plaintext, in order.
void testMultipleFramesRoundTrip() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	std::vector<binary> frames = {makeFrame(150, 0x01), makeFrame(2500, 0x02),
	                              makeFrame(300, 0x03), makeFrame(4096, 0x04)};

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), frames);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.size() == frames.size(), "expected " + std::to_string(frames.size()) +
	                                           " decrypted frames, got " +
	                                           std::to_string(decoded.size()));
	for (size_t i = 0; i < frames.size(); ++i)
		check(sameBytes(frames[i], decoded[i]),
		      "frame " + std::to_string(i) + " did not round-trip");
}

// The same round-trip must work across cipher suites (CTR+HMAC, AES-128-GCM,
// AES-256-GCM).
void testCipherSuitesRoundTrip() {
	for (uint8_t suite : {uint8_t(0x01), uint8_t(0x04), uint8_t(0x05)}) {
		auto sessionKey = makeSessionKey(suite);
		auto frame = makeFrame(1800, static_cast<uint8_t>(0x30 + suite));

		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {frame});
		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(packets, provider);
		check(decoded.size() == 1, "suite " + std::to_string(suite) +
		                               ": expected 1 decrypted frame, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()),
		      "suite " + std::to_string(suite) + ": frame did not round-trip");
	}
}

// A tampered ciphertext must fail authentication and be dropped, while a good
// frame sent alongside it is still recovered.
void testTamperedFrameDropped() {
	const uint8_t suite = 0x04; // AES-128-GCM
	auto sessionKey = makeSessionKey(suite);
	auto goodFrame = makeFrame(180, 0x55);
	auto badFrame = makeFrame(190, 0x66);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {goodFrame, badFrame});

	// The good frame uses RTP timestamp 1000, the second frame 4000. Corrupt
	// the last byte (inside the auth tag) of the second frame's packet.
	bool tampered = false;
	for (auto &pkt : packets) {
		auto header = reinterpret_cast<const RtpHeader *>(pkt->data());
		if (header->timestamp() == 4000 && pkt->size() > 0) {
			(*pkt)[pkt->size() - 1] ^= std::byte{0xFF};
			tampered = true;
		}
	}
	check(tampered, "test setup: failed to find the second frame's packet to tamper");

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.size() == 1,
	      "expected only the good frame to survive, got " + std::to_string(decoded.size()));
	check(sameBytes(goodFrame, decoded.front()), "good frame did not round-trip");
}

// Audio, single RTP packet: a small frame that fits one packet, round-tripped
// through SFrameAudioRtpDepacketizer at an audio clock rate.
void testAudioFrameRoundTrip() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000; // Opus
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(160, 0x77);

	auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey), {frame});
	check(packets.size() == 1, "audio frame should produce one RTP packet, got " +
	                               std::to_string(packets.size()));
	checkFrameMarkers(packets);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeAudio(packets, clockRate, provider);
	check(decoded.size() == 1, "expected 1 decrypted audio frame, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "audio frame did not round-trip");
}

// The packetizer fragments audio on the same MTU boundary as video, so an audio frame
// above that size arrives as an S packet followed by one ending in E. A depacketizer that
// only accepted single-packet S|E frames would drop both halves and lose every large
// frame, silently.
void testAudioMultiPacketReassembly() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000; // Opus
	auto sessionKey = makeSessionKey(suite);
	auto large = makeFrame(2000, 0x31); // above one chunk, like a max-size Opus frame
	auto small = makeFrame(160, 0x32);

	// A large frame really does fragment, and reassembles
	{
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey), {large});
		check(packets.size() > 1, "a 2000 byte audio frame should fragment, got " +
		                              std::to_string(packets.size()) + " packet(s)");

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1, "expected 1 reassembled audio frame, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(large, decoded.front()), "large audio frame did not round-trip");
	}

	// Small and large interleaved, all delivered and in order
	{
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey),
		                               {small, large, small});
		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 3, "expected 3 audio frames, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(small, decoded[0]), "first small frame did not round-trip");
		check(sameBytes(large, decoded[1]), "large frame did not round-trip");
		check(sameBytes(small, decoded[2]), "second small frame did not round-trip");
	}

	// Losing the S packet must drop only that frame, not poison the next one
	{
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey),
		                               {large, small});
		check(packets.size() > 2, "expected the large frame to fragment");
		packets.erase(packets.begin()); // drop the S packet of the large frame

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1, "expected only the small frame to survive, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(small, decoded.front()), "the following frame did not round-trip");
	}

	// Losing the E packet must drop that frame and still deliver the next
	{
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey),
		                               {large, small});
		// The large frame's last packet is the one before the small frame's single packet
		check(packets.size() > 2, "expected the large frame to fragment");
		packets.erase(packets.end() - 2);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1, "expected only the small frame to survive, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(small, decoded.front()), "the following frame did not round-trip");
	}
}

// Audio fragments are appended in arrival order, so the sequence number is the only thing
// that can reject one that is out of place: two middle fragments of a frame share a
// timestamp and neither carries S. Splicing them in the wrong order, or across a lost
// packet, produces ciphertext that authentication would reject anyway -- so the frame
// count alone cannot tell the two behaviours apart, and the key-lookup count is what
// shows the frame was refused at reassembly rather than after a wasted derivation.
void testAudioOutOfSequenceFragmentDropped() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000; // Opus
	auto sessionKey = makeSessionKey(suite);
	auto large = makeFrame(4000, 0x33); // several packets: S, middle(s), E
	auto next = makeFrame(160, 0x34);

	// Two middle fragments swapped
	{
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey),
		                               {large, next});
		check(packets.size() >= 5, "expected the large frame to span at least 4 packets, got " +
		                               std::to_string(packets.size()));
		std::swap(packets[1], packets[2]);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1, "expected only the following frame, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(next, decoded.front()), "the following frame did not round-trip");
		check(provider->lookups() == 1,
		      "expected 1 key lookup for the intact frame, got " +
		          std::to_string(provider->lookups()) +
		          ": the reordered frame was spliced and sent to decryption instead of "
		          "being rejected from its sequence numbers");
	}

	// A lost middle fragment leaves a gap
	{
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey),
		                               {large, next});
		packets.erase(packets.begin() + 1);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1, "expected only the following frame, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(next, decoded.front()), "the following frame did not round-trip");
		check(provider->lookups() == 1,
		      "expected 1 key lookup for the intact frame, got " +
		          std::to_string(provider->lookups()) +
		          ": the frame with a gap reached key derivation");
	}
}

void testVideoReorderAcrossFrameBoundary() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frameA = makeFrame(1600, 0x41); // two packets each
	auto frameB = makeFrame(1600, 0x42);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frameA, frameB});
	check(packets.size() == 4, "expected 2 packets per frame, got " +
	                               std::to_string(packets.size()));

	// a(S) c(S) b(E) d(E): the second frame's first packet overtakes the first frame's last
	message_vector reordered = {packets[0], packets[2], packets[1], packets[3]};

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(reordered, provider);
	check(decoded.size() == 2, "expected both frames to survive the reorder, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(frameA, decoded[0]), "first frame did not round-trip after reorder");
	check(sameBytes(frameB, decoded[1]), "second frame did not round-trip after reorder");
}

// A frame missing a middle packet must be dropped, not concatenated spliced, and it must
// not block the frames behind it.
void testVideoFrameWithMissingPacketDropped() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto big = makeFrame(3600, 0x51); // three packets
	auto next = makeFrame(400, 0x52); // one packet

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {big, next});
	check(packets.size() >= 4, "expected the large frame to span several packets");

	// Drop the middle packet of the first frame
	message_vector damaged;
	for (size_t i = 0; i < packets.size(); ++i)
		if (i != 1)
			damaged.push_back(packets[i]);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(damaged, provider);
	check(decoded.size() == 1, "expected only the intact frame, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(next, decoded.front()),
	      "the frame after the damaged one did not round-trip");

	// The gap is visible in the RTP sequence numbers, so the frame should be rejected
	// before any key material is derived. Authentication would also reject the spliced
	// ciphertext, but only after a wasted HKDF derivation and decrypt -- and the key
	// lookup is the observable difference between the two.
	check(provider->lookups() == 1,
	      "expected 1 key lookup for the intact frame, got " +
	          std::to_string(provider->lookups()) +
	          ": the damaged frame reached key derivation instead of being rejected "
	          "from its sequence numbers");
}

// Per-packet SFrame (draft-ietf-avtcore-rtp-sframe section 3): each RTP packet carries a
// complete, independently encrypted SFrame object, so its descriptor sets both S and E and
// several objects share one RTP timestamp. Section 5.2 delimits objects by the descriptor
// bits rather than by timestamp, so every object must be decrypted and delivered.
void testVideoPerPacketSFrame() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	// Three small objects, one packet each, every one its own SFrame ciphertext with its
	// own counter.
	std::vector<binary> objects = {makeFrame(200, 0x61), makeFrame(200, 0x62),
	                               makeFrame(200, 0x63)};
	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), objects);
	check(packets.size() == 3,
	      "expected one packet per object, got " + std::to_string(packets.size()));

	// Collapse them onto one RTP timestamp with consecutive sequence numbers, the marker
	// only on the last: one video frame carried as three per-packet SFrame objects.
	message_vector perPacket;
	for (size_t i = 0; i < packets.size(); ++i) {
		check(descriptorByte(packets[i]) == uint8_t(kSFrameDescriptorS | kSFrameDescriptorE),
		      "each per-packet object should carry S|E");
		perPacket.push_back(
		    withRtpFraming(packets[i], 9000, uint16_t(100 + i), i + 1 == packets.size()));
	}

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(perPacket, provider);
	check(decoded.size() == 3,
	      "expected 3 per-packet SFrame objects, got " + std::to_string(decoded.size()));
	for (size_t i = 0; i < objects.size(); ++i)
		check(sameBytes(objects[i], decoded[i]),
		      "per-packet object " + std::to_string(i) + " did not round-trip");
}

// One timestamp can hold objects of different lengths, so the run delimiter has to be the
// S/E pair: a two-packet object followed by a one-packet object under a single timestamp
// must yield exactly those two objects, in order.
void testVideoMixedRunLengthsInOneTimestamp() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto big = makeFrame(1600, 0x71);  // two packets
	auto small = makeFrame(200, 0x72); // one packet

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {big, small});
	check(packets.size() == 3,
	      "expected 2 packets then 1, got " + std::to_string(packets.size()));

	message_vector merged;
	for (size_t i = 0; i < packets.size(); ++i)
		merged.push_back(
		    withRtpFraming(packets[i], 12000, uint16_t(500 + i), i + 1 == packets.size()));

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(merged, provider);
	check(decoded.size() == 2,
	      "expected 2 objects from the mixed timestamp group, got " +
	          std::to_string(decoded.size()));
	check(sameBytes(big, decoded[0]), "the two-packet object did not round-trip");
	check(sameBytes(small, decoded[1]), "the one-packet object did not round-trip");
}

// A single-packet per-frame object and a true per-packet object both carry S|E, so the T bit
// is the only thing on the wire that separates them. One audio packet is decoded with T=0 and
// dropped with T=1, from otherwise identical bytes: the receiver can tell the two apart, and
// refuses the mode it cannot honour rather than handing a packetized payload upstream as if
// it were a whole frame.
void testPacketizedOriginDistinguishedFromSinglePacketFrame() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(160, 0x81); // comfortably inside one packet

	auto build = [&] {
		auto packets =
		    packetizeFrames(48000, makeSFrameConfig(suite, sessionKey), {frame});
		check(packets.size() == 1, "expected a single audio packet");
		return packets;
	};

	// T=0: an ordinary per-frame object that happens to fit one packet.
	{
		auto packets = build();
		check(descriptorByte(packets.front()) ==
		          uint8_t(kSFrameDescriptorS | kSFrameDescriptorE),
		      "a single-packet per-frame object should be S|E with T=0");

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(std::move(packets), 48000, provider);
		check(decoded.size() == 1,
		      "expected the single-packet frame to decode, got " + std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()), "single-packet frame did not round-trip");
	}

	// T=1: the same bytes declared as packetized origin must be refused.
	{
		auto packets = build();
		setDescriptorByte(packets.front(), uint8_t(kSFrameDescriptorS | kSFrameDescriptorE |
		                                          kSFrameDescriptorT));

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(std::move(packets), 48000, provider);
		check(decoded.empty(), "a packetized-origin (T=1) packet should be dropped, got " +
		                           std::to_string(decoded.size()));
		check(provider->lookups() == 0,
		      "a T=1 packet should be refused on its descriptor, before any key derivation");
	}
}

// RTCP is not SFrame-protected, so both depacketizers must hand Control messages through
// untouched. Track::incoming delivers one message per call, so RTCP always arrives in a
// batch of its own with no media alongside -- meaning a receive path that fails closed on
// "no SSRC in this batch" by clearing everything destroys every SR, RR and APP packet.
void testRtcpPassesThroughDepacketizers() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	// Sender report, receiver report, application-defined.
	for (uint8_t packetType : {uint8_t(200), uint8_t(201), uint8_t(204)}) {
		binary rtcp(28, std::byte{0x5A});
		rtcp[0] = std::byte{0x80}; // V=2
		rtcp[1] = std::byte{packetType};
		rtcp[2] = std::byte{0x00};
		rtcp[3] = std::byte{0x06};

		const std::string what = "RTCP type " + std::to_string(packetType);

		{
			message_vector batch = {make_message(rtcp.begin(), rtcp.end(), Message::Control)};
			SFrameVideoRtpDepacketizer d(std::make_shared<SessionKeyProvider>(suite, sessionKey));
			d.incoming(batch, [](message_ptr) {});
			check(batch.size() == 1, what + " was dropped by the video depacketizer");
			check(batch.front()->type == Message::Control,
			      what + " lost its Control type in the video depacketizer");
			check(binary(batch.front()->begin(), batch.front()->end()) == rtcp,
			      what + " was modified by the video path");
		}

		{
			message_vector batch = {make_message(rtcp.begin(), rtcp.end(), Message::Control)};
			SFrameAudioRtpDepacketizer d(48000,
			                             std::make_shared<SessionKeyProvider>(suite, sessionKey));
			d.incoming(batch, [](message_ptr) {});
			check(batch.size() == 1, what + " was dropped by the audio depacketizer");
			check(batch.front()->type == Message::Control,
			      what + " lost its Control type in the audio depacketizer");
			check(binary(batch.front()->begin(), batch.front()->end()) == rtcp,
			      what + " was modified by the audio path");
		}
	}

	// Mixed batch: the media is decrypted and the RTCP still survives alongside it.
	{
		auto frame = makeFrame(300, 0x91);
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {frame});
		binary rtcp(28, std::byte{0x77});
		rtcp[0] = std::byte{0x80};
		rtcp[1] = std::byte{201};
		rtcp[3] = std::byte{0x06};
		packets.insert(packets.begin(), make_message(rtcp.begin(), rtcp.end(), Message::Control));

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto out = depacketizeVideo(packets, provider);
		int control = 0, media = 0;
		for (const auto &m : out)
			(m->type == Message::Control ? control : media)++;
		check(control == 1, "expected the RTCP packet alongside the media, got " +
		                        std::to_string(control));
		check(media == 1, "expected 1 decrypted frame, got " + std::to_string(media));
	}
}

// Shared keying (no per-SSRC derivation) means every track sends under one key, so the
// counter has to be shared too: two tracks each starting at ctrStart would emit the same
// (key, CTR) and so the same nonce, which on the GCM suites leaks the GHASH key. The API
// makes that unrepresentable by requiring the encoder to be passed in, and this pins that
// the counters really are one sequence.
void testSharedKeyCountersAreUnique() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	SFrameConfig config;
	config.keyDetails = SFrameKeyDetails{suite, sessionKey, 0, SFrameKeyUse::Encrypt};
	config.keyGeneration = 1;
	auto encoder = std::make_shared<SFrameEncoder>(config);

	// Two tracks, different SSRCs, one encoder.
	auto videoConfig = makeConfig(RtpPacketizer::VideoClockRate);
	videoConfig->ssrc = 1111;
	auto audioConfig = makeConfig(48000);
	audioConfig->ssrc = 2222;

	SFrameRtpPacketizer videoPacketizer(videoConfig, encoder, SFrameMode::PerFrame);
	SFrameRtpPacketizer audioPacketizer(audioConfig, encoder, SFrameMode::PerFrame);

	std::set<uint64_t> counters;
	const size_t framesPerTrack = 20;
	for (size_t i = 0; i < framesPerTrack; ++i) {
		for (auto *packetizer : {&videoPacketizer, &audioPacketizer}) {
			auto frameInfo = std::make_shared<FrameInfo>(uint32_t(3000 * (i + 1)));
			frameInfo->payloadType = TEST_PT;
			message_vector messages = {
			    make_message(makeFrame(200, uint8_t(i)), frameInfo)};
			packetizer->outgoing(messages, [](message_ptr) {});

			for (const auto &m : messages) {
				size_t hdrSize = 0, payloadEnd = 0;
				if (!impl::SFrameUtility::ParseSFramePacket(m, hdrSize, payloadEnd))
					continue;
				// Skip the descriptor byte to reach the SFrame header.
				binary object(m->begin() + hdrSize + 1, m->begin() + payloadEnd);
				if (object.empty())
					continue;
				counters.insert(impl::SFrameHeader::Decode(object).ctr);
				break; // one object per frame
			}
		}
	}

	check(counters.size() == framesPerTrack * 2,
	      "expected " + std::to_string(framesPerTrack * 2) +
	          " distinct counters across two tracks sharing a key, got " +
	          std::to_string(counters.size()) +
	          ": the tracks are not sharing one counter, so they reuse nonces");
}

// Shared keying has to actually work end to end, not just allocate counters correctly: the
// receiver must skip the per-SSRC derivation exactly as the sender did, or it derives a
// different key and nothing authenticates.
class SharedKeyProviderNoDerivation final : public SFrameKeyProvider {
public:
	SharedKeyProviderNoDerivation(uint8_t suite, binary key)
	    : mSuite(suite), mKey(std::move(key)) {}

	bool usePerSSRCDerivation() const override { return false; }
	uint8_t ratchetStepBits() const override { return 0; }

	std::optional<SFrameKeyDetails> getKeyDetails(uint64_t) override {
		return SFrameKeyDetails{mSuite, mKey, 0, SFrameKeyUse::Decrypt};
	}

private:
	const uint8_t mSuite;
	const binary mKey;
};

void testSharedKeyRoundTrip() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	SFrameConfig config;
	config.keyDetails = SFrameKeyDetails{suite, sessionKey, 0, SFrameKeyUse::Encrypt};
	config.keyGeneration = 1;
	auto encoder = std::make_shared<SFrameEncoder>(config);

	auto rtpConfig = makeConfig(RtpPacketizer::VideoClockRate);
	SFrameRtpPacketizer packetizer(rtpConfig, encoder, SFrameMode::PerFrame);

	auto frame = makeFrame(1500, 0xA7); // large enough to fragment
	auto frameInfo = std::make_shared<FrameInfo>(3000);
	frameInfo->payloadType = TEST_PT;
	message_vector packets = {make_message(binary(frame), frameInfo)};
	packetizer.outgoing(packets, [](message_ptr) {});
	check(packets.size() > 1, "expected the frame to fragment");

	auto provider = std::make_shared<SharedKeyProviderNoDerivation>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.size() == 1,
	      "expected 1 frame under shared keying, got " + std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "shared-key frame did not round-trip");
}

// The two sides must agree on whether Section 7 applies. A mismatch is the quiet failure:
// the KID, the descriptor and the framing all look right, and only the AEAD tag rejects it.
void testKeyScopeMismatchFails() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	// Sender derives per SSRC, as the config constructor does.
	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {makeFrame(400, 0xB8)});

	// Receiver does not, so it derives the base key instead of the per-SSRC one.
	auto provider = std::make_shared<SharedKeyProviderNoDerivation>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.empty(),
	      "a frame decoded even though the sender derived per SSRC and the receiver did not");
}

// Malformed RTP packets pushed through the depacketizer must be filtered out
// by the length checks (too small for an RTP header + descriptor; header with
// no payload beyond the descriptor) without crashing or corrupting a valid
// frame received alongside them.
void testBadPacketsFiltered() {	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x5A);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frame});
	check(packets.size() == 1, "expected one packet for the valid frame");

	// Too small to hold an RTP header plus the 1-byte S/E/T descriptor.
	binary tooSmall(4, std::byte{0});
	// An RTP header (zeroed: no CSRC/extension) plus the descriptor byte, but no
	// payload beyond it (size == hdrSize + 1).
	binary headerOnly(sizeof(RtpHeader) + 1, std::byte{0});

	message_vector input;
	input.push_back(make_message(std::move(tooSmall), Message::Binary));
	input.push_back(make_message(std::move(headerOnly), Message::Binary));
	input.push_back(packets.front()); // the valid packet (marker set)

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(std::move(input), provider);
	check(decoded.size() == 1, "malformed packets must be dropped; expected 1 frame, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()),
	      "valid frame did not survive alongside malformed packets");
}

// Explicit check on the 1-byte SFrame RFC descriptor (S/E/T) that the
// packetizer prepends to each RTP payload. Guards against a direct change to
// the descriptor encoding in sframeFragmentRFC. The bits are determined by the
// fragment's position and are independent of encryption.
void testDescriptorByteEncoding() {
	const uint8_t S = kSFrameDescriptorS;
	const uint8_t E = kSFrameDescriptorE;
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	// Single packet → the one descriptor carries both start and end.
	{
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {makeFrame(200, 0x10)});
		check(packets.size() == 1, "expected a single packet");
		uint8_t d = descriptorByte(packets.front());
		check(d == (S | E),
		      "single-packet descriptor should be 0xC0 (S|E), got " + std::to_string(d));
	}

	// Multiple packets → S on the first, E on the last, nothing on the middle.
	{
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {makeFrame(5000, 0x20)});
		check(packets.size() > 1, "expected multiple packets");
		for (size_t i = 0; i < packets.size(); ++i) {
			uint8_t d = descriptorByte(packets[i]);
			const bool first = (i == 0);
			const bool last = (i + 1 == packets.size());
			uint8_t expected = uint8_t((first ? S : 0) | (last ? E : 0));
			check(d == expected, "packet " + std::to_string(i) + " descriptor=" +
			                         std::to_string(d) + " expected " + std::to_string(expected));
		}
	}
}

// Malformed SFrame descriptor bits must cause the depacketizer to drop the
// frame (T must be 0, S only on the first packet, E only on the last), so a
// packet with bad descriptor bits is ignored rather than mis-reassembled.
void testBadDescriptorBitsIgnored() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	// Single-packet object: the valid descriptor is 0xC0 (S|E, T=0). Each variant must be
	// rejected: a reserved bit set, T=1 for packetized origin, missing S, missing E, neither.
	for (uint8_t bad : {uint8_t(0xC1), uint8_t(0xE0), uint8_t(0x40), uint8_t(0x80),
	                    uint8_t(0x00)}) {
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {makeFrame(200, 0x40)});
		check(packets.size() == 1, "expected a single packet");
		setDescriptorByte(packets.front(), bad);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(std::move(packets), provider);
		check(decoded.empty(), "single-packet frame with descriptor " + std::to_string(bad) +
		                           " should be dropped, got " + std::to_string(decoded.size()));
	}

	// Multi-packet frame: clear the start bit on the first fragment (should be
	// 0x80). The whole frame must be dropped.
	{
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {makeFrame(5000, 0x50)});
		check(packets.size() > 1, "expected multiple packets");
		setDescriptorByte(packets.front(), 0x00);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(std::move(packets), provider);
		check(decoded.empty(),
		      "multi-packet frame with a bad first descriptor should be dropped");
	}
}

// The audio path validates the descriptor too: a single-packet audio frame
// whose descriptor is not S|E (T=0) must be dropped.
void testAudioBadDescriptorIgnored() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000;
	auto sessionKey = makeSessionKey(suite);

	for (uint8_t bad : {uint8_t(0xC1), uint8_t(0x80), uint8_t(0x40), uint8_t(0x00)}) {
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey),
		                               {makeFrame(160, 0x70)});
		check(packets.size() == 1, "expected a single audio packet");
		setDescriptorByte(packets.front(), bad);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(std::move(packets), clockRate, provider);
		check(decoded.empty(), "audio packet with descriptor " + std::to_string(bad) +
		                           " should be dropped, got " + std::to_string(decoded.size()));
	}
}

// RTP header extensions must be accounted for when locating the 1-byte SFrame
// descriptor and payload. Enable a MID header extension so every RTP packet
// carries an extension block, and confirm frames still round-trip — the
// depacketizer must skip RTP header + extension + descriptor correctly.
void testHeaderExtensionsAccounted() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	auto runWithMid = [&](uint32_t clockRate, const binary &frame, bool audio) {
		auto config = std::make_shared<RtpPacketizationConfig>(TEST_SSRC, TEST_CNAME, TEST_PT,
		                                                       clockRate);
		config->mid = "0";  // MID value
		config->midId = 1;  // one-byte header extension id (1-14)

		SFrameRtpPacketizer packetizer(config, makeSFrameConfig(suite, sessionKey),
	                               SFrameMode::PerFrame);

		auto frameInfo = std::make_shared<FrameInfo>(1000);
		frameInfo->payloadType = TEST_PT;
		message_vector packets{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(packets, [](message_ptr) {});

		// Sanity: every packet must actually carry an RTP header extension,
		// otherwise this test would not exercise extension accounting.
		for (const auto &pkt : packets) {
			auto h = reinterpret_cast<const RtpHeader *>(pkt->data());
			check(h->getExtensionHeaderSize() > 0,
			      "expected an RTP header extension on every packet");
		}

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = audio ? depacketizeAudio(std::move(packets), clockRate, provider)
		                     : depacketizeVideo(std::move(packets), provider);
		check(decoded.size() == 1, "expected 1 decrypted frame with extensions, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()),
		      "frame with an RTP header extension did not round-trip");
	};

	// Video, multiple packets: extension on every fragment.
	runWithMid(RtpPacketizer::VideoClockRate, makeFrame(5000, 0x61), /*audio=*/false);
	// Video, single packet.
	runWithMid(RtpPacketizer::VideoClockRate, makeFrame(200, 0x62), /*audio=*/false);
	// Audio, single packet.
	runWithMid(48000, makeFrame(160, 0x63), /*audio=*/true);
}

// CSRC identifiers must be accounted for in the header size. The packetizer
// never emits CSRCs, so inject them by hand and confirm the frame still
// round-trips (the depacketizer must skip fixed header + CSRC list + descriptor).
void testCsrcAccounted() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	auto runWithCsrcs = [&](uint32_t clockRate, const binary &frame, bool audio) {
		auto packets = packetizeFrames(clockRate, makeSFrameConfig(suite, sessionKey), {frame});
		check(packets.size() == 1, "expected a single packet to inject CSRCs into");

		auto withCsrc = withCsrcs(packets.front(), 3);
		auto h = reinterpret_cast<const RtpHeader *>(withCsrc->data());
		check(h->csrcCount() == 3, "expected 3 CSRCs on the rewritten packet");

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		message_vector input{withCsrc};
		auto decoded = audio ? depacketizeAudio(std::move(input), clockRate, provider)
		                     : depacketizeVideo(std::move(input), provider);
		check(decoded.size() == 1, "expected 1 decrypted frame with CSRCs, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()), "frame with CSRCs did not round-trip");
	};

	runWithCsrcs(RtpPacketizer::VideoClockRate, makeFrame(200, 0x64), /*audio=*/false);
	runWithCsrcs(48000, makeFrame(160, 0x65), /*audio=*/true);
}

// Combination: a packet carrying BOTH multiple CSRCs and a header extension.
// The header size must account for fixed header + CSRC list + extension block
// together before the descriptor/payload.
void testCsrcAndExtensionAccounted() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	auto config = std::make_shared<RtpPacketizationConfig>(TEST_SSRC, TEST_CNAME, TEST_PT,
	                                                       RtpPacketizer::VideoClockRate);
	config->mid = "0";
	config->midId = 1;

	SFrameRtpPacketizer packetizer(config, makeSFrameConfig(suite, sessionKey),
	                               SFrameMode::PerFrame);

	auto frame = makeFrame(200, 0x66);
	auto frameInfo = std::make_shared<FrameInfo>(1000);
	frameInfo->payloadType = TEST_PT;
	message_vector produced{make_message(binary(frame), frameInfo)};
	packetizer.outgoing(produced, [](message_ptr) {});
	check(produced.size() == 1, "expected a single packet with an extension");

	// Inject CSRCs into the packet that already carries a header extension.
	auto pkt = withCsrcs(produced.front(), 2);
	auto h = reinterpret_cast<const RtpHeader *>(pkt->data());
	check(h->csrcCount() == 2 && h->getExtensionHeaderSize() > 0,
	      "expected 2 CSRCs and a header extension on the packet");

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	message_vector input{pkt};
	auto decoded = depacketizeVideo(std::move(input), provider);
	check(decoded.size() == 1, "expected 1 decrypted frame with CSRCs + extension, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()),
	      "frame with CSRCs and a header extension did not round-trip");
}

// Multiple RTP header extensions of different lengths (and the two-byte header
// format) must be skipped as one block. The packetizer packs several elements
// into the extension block; the depacketizer skips the whole block via its
// 4-byte-aligned length.
void testMultipleHeaderExtensionsAccounted() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	auto roundTrip = [&](shared_ptr<RtpPacketizationConfig> config, const binary &frame,
	                     bool audio) -> size_t {
		SFrameRtpPacketizer packetizer(config, makeSFrameConfig(suite, sessionKey),
	                               SFrameMode::PerFrame);

		auto frameInfo = std::make_shared<FrameInfo>(1000);
		frameInfo->payloadType = TEST_PT;
		message_vector packets{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(packets, [](message_ptr) {});

		size_t extSize =
		    reinterpret_cast<const RtpHeader *>(packets.front()->data())->getExtensionHeaderSize();
		check(extSize > 0, "expected a header extension block");

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = audio ? depacketizeAudio(std::move(packets), config->clockRate, provider)
		                     : depacketizeVideo(std::move(packets), provider);
		check(decoded.size() == 1, "expected 1 frame with header extensions, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()),
		      "frame with multiple header extensions did not round-trip");
		return extSize;
	};

	auto makeCfg = [&](uint32_t clockRate) {
		return std::make_shared<RtpPacketizationConfig>(TEST_SSRC, TEST_CNAME, TEST_PT, clockRate);
	};

	// Several one-byte-header extensions of different lengths in one block:
	// MID (1 byte) + RID (2 bytes) + playout delay (3 bytes).
	auto cfgMulti = makeCfg(RtpPacketizer::VideoClockRate);
	cfgMulti->midId = 1;
	cfgMulti->mid = "0";
	cfgMulti->ridId = 2;
	cfgMulti->rid = "hi";
	cfgMulti->playoutDelayId = 3;
	size_t multiSize = roundTrip(cfgMulti, makeFrame(300, 0x71), /*audio=*/false);

	// A single MID extension, for comparison — the multi-element block must be
	// larger, proving the extra elements were actually present and skipped.
	auto cfgSingle = makeCfg(RtpPacketizer::VideoClockRate);
	cfgSingle->midId = 1;
	cfgSingle->mid = "0";
	size_t singleSize = roundTrip(cfgSingle, makeFrame(300, 0x72), /*audio=*/false);
	check(multiSize > singleSize,
	      "multi-element extension block should be larger than a single element");

	// Two-byte header extension format (an id > 14 forces the two-byte form).
	auto cfgTwoByte = makeCfg(RtpPacketizer::VideoClockRate);
	cfgTwoByte->midId = 15;
	cfgTwoByte->mid = "mid-value";
	roundTrip(cfgTwoByte, makeFrame(300, 0x73), /*audio=*/false);

	// The same multi-extension block through the audio path.
	auto cfgAudio = makeCfg(48000);
	cfgAudio->midId = 1;
	cfgAudio->mid = "0";
	cfgAudio->ridId = 2;
	cfgAudio->rid = "hi";
	cfgAudio->playoutDelayId = 3;
	roundTrip(cfgAudio, makeFrame(160, 0x74), /*audio=*/true);
}

// Trailing RTP padding (P bit + pad-count last byte) must be stripped before
// the SFrame payload is decrypted, for various valid padding lengths, on both
// the video and audio paths.
void testPaddingStripped() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	for (uint8_t pad : {uint8_t(1), uint8_t(2), uint8_t(4), uint8_t(16), uint8_t(64), uint8_t(255)}) {
		// Video, single packet.
		{
			auto frame = makeFrame(200, 0x80);
			auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
			                               makeSFrameConfig(suite, sessionKey), {frame});
			check(packets.size() == 1, "expected a single video packet");
			auto padded = withPadding(packets.front(), pad);
			check(reinterpret_cast<const RtpHeader *>(padded->data())->padding(),
			      "P bit should be set on the padded packet");

			auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
			message_vector input{padded};
			auto decoded = depacketizeVideo(std::move(input), provider);
			check(decoded.size() == 1, "video pad=" + std::to_string(pad) +
			                               ": expected 1 frame, got " +
			                               std::to_string(decoded.size()));
			check(sameBytes(frame, decoded.front()),
			      "video pad=" + std::to_string(pad) + ": frame did not round-trip");
		}
		// Audio, single packet.
		{
			auto frame = makeFrame(160, 0x81);
			auto packets =
			    packetizeFrames(48000, makeSFrameConfig(suite, sessionKey), {frame});
			check(packets.size() == 1, "expected a single audio packet");
			auto padded = withPadding(packets.front(), pad);

			auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
			message_vector input{padded};
			auto decoded = depacketizeAudio(std::move(input), 48000, provider);
			check(decoded.size() == 1, "audio pad=" + std::to_string(pad) +
			                               ": expected 1 frame, got " +
			                               std::to_string(decoded.size()));
			check(sameBytes(frame, decoded.front()),
			      "audio pad=" + std::to_string(pad) + ": frame did not round-trip");
		}
	}

	// Multi-packet video frame with padding applied to every fragment.
	{
		auto frame = makeFrame(5000, 0x82);
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {frame});
		check(packets.size() > 1, "expected a fragmented video frame");
		for (auto &pkt : packets)
			pkt = withPadding(pkt, 8);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(std::move(packets), provider);
		check(decoded.size() == 1, "expected 1 reassembled padded frame, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()),
		      "fragmented frame with per-packet padding did not round-trip");
	}
}

// A padding count that exceeds the available payload is malformed and the
// packet must be dropped rather than under-flowing the payload range.
void testInvalidPaddingDropped() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x83);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSFrameConfig(suite, sessionKey), {frame});
	check(packets.size() == 1, "expected a single packet");

	// Set the P bit and claim a padding count larger than the whole packet.
	auto pkt = packets.front();
	(*pkt)[0] = static_cast<std::byte>(static_cast<uint8_t>((*pkt)[0]) | 0x20);
	(*pkt)[pkt->size() - 1] = static_cast<std::byte>(0xFF); // 255 > payload

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	message_vector input{pkt};
	auto decoded = depacketizeVideo(std::move(input), provider);
	check(decoded.empty(),
	      "packet with an over-large padding count must be dropped, got " +
	          std::to_string(decoded.size()));
}

} // namespace

#endif // RTC_ENABLE_MEDIA

// One key provider serving two tracks at once. The provider answers for a KID only and
// keeps no per-call state, so the audio and video decoders cannot interfere with each
// other's key derivation however their calls interleave.
void testSharedProviderAcrossTracks() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);

	auto videoFrame = makeFrame(400, 0x11);
	auto audioFrame = makeFrame(120, 0x22);
	const uint32_t audioClockRate = 48000;

	// Two tracks with different SSRCs, so a key derived for one cannot decrypt the other.
	auto videoPackets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                                    makeSFrameConfig(suite, sessionKey), {videoFrame});
	auto audioPackets = packetizeFramesWithSsrc(audioClockRate, TEST_SSRC + 1,
	                                            makeSFrameConfig(suite, sessionKey),
	                                            {audioFrame});

	SFrameVideoRtpDepacketizer videoDepack(provider);
	SFrameAudioRtpDepacketizer audioDepack(audioClockRate, provider);

	// Interleave the two tracks through the one provider.
	for (int round = 0; round < 3; ++round) {
		auto v = videoPackets;
		auto a = audioPackets;
		videoDepack.incoming(v, [](message_ptr) {});
		audioDepack.incoming(a, [](message_ptr) {});

		check(v.size() == 1, "video frame lost in round " + std::to_string(round) + ", got " +
		                         std::to_string(v.size()));
		check(sameBytes(videoFrame, v.front()),
		      "video frame did not round-trip in round " + std::to_string(round));
		check(a.size() == 1, "audio frame lost in round " + std::to_string(round) + ", got " +
		                         std::to_string(a.size()));
		check(sameBytes(audioFrame, a.front()),
		      "audio frame did not round-trip in round " + std::to_string(round));
	}
}

// The decoder caches derived key material per KID, so the cache must not outlive the key
// it came from: a provider that re-keys a KID in place has to take effect immediately.
void testDerivedKeyCacheInvalidation() {
	const uint8_t suite = 0x04;
	auto firstKey = makeSessionKey(suite);
	auto secondKey = firstKey;
	secondKey[0] ^= std::byte{0xFF}; // a different master key under the same KID

	auto frame = makeFrame(256, 0x33);
	auto firstPackets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                                    makeSFrameConfig(suite, firstKey), {frame});
	auto secondPackets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                                     makeSFrameConfig(suite, secondKey), {frame});

	auto provider = std::make_shared<MutableKeyProvider>(suite, firstKey);
	SFrameVideoRtpDepacketizer depacketizer(provider);

	// Warm the cache on the first key
	auto a = firstPackets;
	depacketizer.incoming(a, [](message_ptr) {});
	check(a.size() == 1 && sameBytes(frame, a.front()), "first key did not round-trip");

	// Re-key the same KID: the cached material is now stale and must be discarded
	provider->setKey(secondKey);
	auto b = secondPackets;
	depacketizer.incoming(b, [](message_ptr) {});
	check(b.size() == 1 && sameBytes(frame, b.front()),
	      "decoder kept using the stale cached key after a re-key");

	// A frame under the old key must no longer decrypt
	auto c = firstPackets;
	depacketizer.incoming(c, [](message_ptr) {});
	check(c.empty(), "a frame under the revoked key still decrypted");
}

// RTCP is not SFrame encoded: it must come back out byte for byte, while media alongside
// it still gets encrypted and packetised.
void testControlMessagesPassThrough() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x11);

	// A stand-in RTCP packet: contents are opaque to the packetizer.
	binary rtcp;
	for (uint8_t i = 0; i < 28; ++i)
		rtcp.push_back(static_cast<std::byte>(0x80 + i));

	SFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
	                               makeSFrameConfig(suite, sessionKey), SFrameMode::PerFrame);

	auto frameInfo = std::make_shared<FrameInfo>(1000);
	frameInfo->payloadType = TEST_PT;

	message_vector messages;
	messages.push_back(make_message(binary(frame), frameInfo));
	messages.push_back(make_message(binary(rtcp), Message::Control));

	packetizer.outgoing(messages, [](message_ptr) {});

	// Exactly one Control message survives, byte for byte
	size_t controlCount = 0;
	for (const auto &msg : messages) {
		if (msg->type != Message::Control)
			continue;
		controlCount++;
		check(msg->size() == rtcp.size(), "RTCP packet was resized, got " +
		                                      std::to_string(msg->size()) + " expected " +
		                                      std::to_string(rtcp.size()));
		check(std::equal(rtcp.begin(), rtcp.end(), msg->begin()),
		      "RTCP packet was modified in the outgoing chain");
	}
	check(controlCount == 1,
	      "expected 1 RTCP packet to survive, got " + std::to_string(controlCount));

	// And the media frame still went through encryption and packetisation
	size_t mediaCount = 0;
	for (const auto &msg : messages)
		if (msg->type != Message::Control)
			mediaCount++;
	check(mediaCount >= 1, "media frame was lost alongside the RTCP packet");

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	message_vector mediaOnly;
	for (auto &msg : messages)
		if (msg->type != Message::Control)
			mediaOnly.push_back(msg);

	auto decoded = depacketizeVideo(mediaOnly, provider);
	check(decoded.size() == 1, "expected 1 decrypted frame, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "media frame did not round-trip");
}

// Whether SFrame framing applies is a property of the packetizer, not of the RTP config
// it was handed. A flag on the shared config would survive the packetizer that set it and
// silently switch a codec packetizer given the same config over to SFrame framing.
void testSFrameFramingNotStickyOnConfig() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto config = makeConfig(RtpPacketizer::VideoClockRate);

	{
		SFrameRtpPacketizer sframePacketizer(config, makeSFrameConfig(suite, sessionKey),
		                                     SFrameMode::PerFrame);
	}

	// The same config now drives a plain packetizer: its payload must be the frame as
	// given, with no SFrame descriptor byte prepended.
	auto frame = makeFrame(64, 0x11);
	AudioRtpPacketizer<48000> plainPacketizer(config);

	auto frameInfo = std::make_shared<FrameInfo>(1000);
	frameInfo->payloadType = TEST_PT;
	message_vector messages;
	messages.push_back(make_message(binary(frame), frameInfo));
	plainPacketizer.outgoing(messages, [](message_ptr) {});

	check(messages.size() == 1, "expected 1 RTP packet, got " + std::to_string(messages.size()));

	auto pkt = reinterpret_cast<const RtpHeader *>(messages.front()->data());
	auto hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
	const size_t payloadSize = messages.front()->size() - hdrSize;
	check(payloadSize == frame.size(),
	      "plain packetizer emitted " + std::to_string(payloadSize) + " payload bytes for a " +
	          std::to_string(frame.size()) + " byte frame, so SFrame framing leaked onto it");
	check(std::equal(frame.begin(), frame.end(), messages.front()->begin() + hdrSize),
	      "plain packetizer payload does not match the frame it was given");
}

// A caller asking for a smaller MTU must get it: the fragment size is honoured rather
// than defaulted, and the descriptor byte counts against the limit.
void testMaxFragmentSizeHonoured() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	const size_t maxFragmentSize = 200;
	auto frame = makeFrame(1500, 0x22);

	SFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
	                               makeSFrameConfig(suite, sessionKey), SFrameMode::PerFrame,
	                               maxFragmentSize);

	auto frameInfo = std::make_shared<FrameInfo>(1000);
	frameInfo->payloadType = TEST_PT;
	message_vector messages;
	messages.push_back(make_message(binary(frame), frameInfo));
	packetizer.outgoing(messages, [](message_ptr) {});

	check(messages.size() > 1, "a 1500 byte frame should fragment at a 200 byte MTU, got " +
	                               std::to_string(messages.size()) + " packets");

	for (const auto &msg : messages) {
		auto pkt = reinterpret_cast<const RtpHeader *>(msg->data());
		auto hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
		const size_t payloadSize = msg->size() - hdrSize;
		check(payloadSize <= maxFragmentSize,
		      "payload of " + std::to_string(payloadSize) + " bytes exceeds the " +
		          std::to_string(maxFragmentSize) + " byte limit");
	}

	// Still decrypts, so the smaller chunks reassemble correctly
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(messages, provider);
	check(decoded.size() == 1, "expected 1 decrypted frame, got " +
	                               std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "fragmented frame did not round-trip");
}

// A sender must not prefix descriptors unless the peer agreed to a=sframe -- a standard
// endpoint reads that byte as codec payload -- and renegotiation must not duplicate the line.
void testSFrameNegotiationAttribute() {
	// addSFrame() is idempotent across renegotiation
	{
		Description::Video media("0", Description::Direction::SendOnly);
		media.addVideoCodec(TEST_PT, "H264");
		media.addSFrame();
		media.addSFrame();
		media.addSFrame();

		const std::string sdp = std::string(media);
		size_t count = 0;
		for (size_t pos = sdp.find("a=sframe"); pos != std::string::npos;
		     pos = sdp.find("a=sframe", pos + 1))
			count++;

		check(media.hasSFrame(), "addSFrame() did not set the attribute");
		check(count == 1, "expected 1 a=sframe line, got " + std::to_string(count));
	}

	// The packetizer consults hasSFrame() to decide whether SFrame applies to this
	// m-line, and can be switched back on if a later description carries the attribute.
	{
		const uint8_t suite = 0x04;
		auto sessionKey = makeSessionKey(suite);
		auto frame = makeFrame(120, 0x66);

		SFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
		                               makeSFrameConfig(suite, sessionKey), SFrameMode::PerFrame);

		auto payloadOf = [&](const Description::Media &desc) {
			packetizer.media(desc);
			auto frameInfo = std::make_shared<FrameInfo>(1000);
			frameInfo->payloadType = TEST_PT;
			message_vector messages;
			messages.push_back(make_message(binary(frame), frameInfo));
			packetizer.outgoing(messages, [](message_ptr) {});
			check(messages.size() == 1, "expected 1 RTP packet");
			auto pkt = reinterpret_cast<const RtpHeader *>(messages.front()->data());
			auto hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
			return messages.front()->size() - hdrSize;
		};

		Description::Video negotiated("0", Description::Direction::SendOnly);
		negotiated.addVideoCodec(TEST_PT, "H264");
		const size_t declinedSize = payloadOf(negotiated);
		check(declinedSize == frame.size(),
		      "declined: payload is " + std::to_string(declinedSize) + " bytes for a " +
		          std::to_string(frame.size()) + " byte frame, SFrame was still applied");

		negotiated.addSFrame();
		const size_t acceptedSize = payloadOf(negotiated);
		check(acceptedSize > frame.size(),
		      "accepted: payload is " + std::to_string(acceptedSize) +
		          " bytes, SFrame was not re-enabled");
	}

	// A peer declining SFrame must disable the handler, not just warn: the send side has
	// to stop encrypting and stop prefixing the descriptor, or the peer receives frames
	// it cannot parse.
	{
		const uint8_t suite = 0x04;
		auto sessionKey = makeSessionKey(suite);
		auto frame = makeFrame(120, 0x77);

		SFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
		                               makeSFrameConfig(suite, sessionKey), SFrameMode::PerFrame);

		Description::Video declined("0", Description::Direction::SendOnly);
		declined.addVideoCodec(TEST_PT, "H264");
		packetizer.media(declined);

		auto frameInfo = std::make_shared<FrameInfo>(1000);
		frameInfo->payloadType = TEST_PT;
		message_vector messages;
		messages.push_back(make_message(binary(frame), frameInfo));
		packetizer.outgoing(messages, [](message_ptr) {});

		check(messages.size() == 1, "expected 1 RTP packet, got " +
		                                std::to_string(messages.size()));

		auto pkt = reinterpret_cast<const RtpHeader *>(messages.front()->data());
		auto hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
		const size_t payloadSize = messages.front()->size() - hdrSize;
		check(payloadSize == frame.size(),
		      "payload is " + std::to_string(payloadSize) + " bytes for a " +
		          std::to_string(frame.size()) + " byte frame: the handler still applied SFrame");
		check(std::equal(frame.begin(), frame.end(), messages.front()->begin() + hdrSize),
		      "payload was modified even though the peer declined SFrame");
	}
}

// SFrameMode is required at every call site and only PerFrame is implemented, so a value
// this code cannot honour must be refused rather than quietly treated as per-frame. Reaching
// the guard needs a cast, since PerPacket is not declared.
void testUnsupportedModeRejected() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	bool threw = false;
	try {
		SFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
		                               makeSFrameConfig(suite, sessionKey),
		                               static_cast<SFrameMode>(1));
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	check(threw, "packetizer accepted an unsupported SFrame mode");

	// PerFrame still constructs, so the guard is not rejecting everything.
	SFrameRtpPacketizer ok(makeConfig(RtpPacketizer::VideoClockRate),
	                       makeSFrameConfig(suite, sessionKey), SFrameMode::PerFrame);
}

void testFailClosedOnMissingKeyMaterial() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	// Send side: an empty base key is rejected at construction
	{
		SFrameConfig config = makeSFrameConfig(suite, sessionKey);
		config.keyDetails.baseKey.clear();

		bool threw = false;
		try {
			SFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate), config,
			                               SFrameMode::PerFrame);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "packetizer accepted an empty base key");
	}

	// Receive side: a null key provider is rejected at construction
	{
		bool threw = false;
		try {
			SFrameVideoRtpDepacketizer depacketizer(nullptr);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "video depacketizer accepted a null key provider");
	}

	{
		bool threw = false;
		try {
			SFrameAudioRtpDepacketizer depacketizer(48000, nullptr);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "audio depacketizer accepted a null key provider");
	}

	// The shared decrypt helper is exported, so it guards itself too — and drops the
	// batch before throwing, so ciphertext is not delivered even if a direct caller
	// swallows the exception.
	{
		auto frame = makeFrame(64, 0x11);
		message_vector messages;
		messages.push_back(make_message(frame.begin(), frame.end()));

		std::unique_ptr<SFrameDecoder> decoder;
		bool threw = false;
		try {
			impl::SFrameUtility::DecryptMessages(messages, nullptr, TEST_SSRC, decoder);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "DecryptMessages accepted a null key provider");
		check(messages.empty(), "DecryptMessages left ciphertext in the batch after failing");
	}

	// The guards must not be satisfiable by a blanket refusal: a correctly configured
	// pair still round-trips.
	{
		auto frame = makeFrame(200, 0x22);
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSFrameConfig(suite, sessionKey), {frame});
		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(packets, provider);
		check(decoded.size() == 1, "expected 1 decrypted frame, got " +
		                               std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()), "frame did not round-trip");
	}
}

TestResult test_sframe_packetizer() {
#if RTC_ENABLE_MEDIA
	InitLogger(LogLevel::Warning);
	try {
		testSmallFrameRoundTrip();
		testFarFutureTimestampDoesNotWedgeVideo();
		testPerSsrcDerivationMustBeSet();
		testPerSsrcDerivationOffRoundTrips();
		testKeyUseMustMatchDirection();
		testLargeFrameRoundTrip();
		testMultipleFramesRoundTrip();
		testCipherSuitesRoundTrip();
		testTamperedFrameDropped();
		testAudioFrameRoundTrip();
		testAudioMultiPacketReassembly();
		testAudioOutOfSequenceFragmentDropped();
		testRtcpPassesThroughDepacketizers();
		testSharedKeyCountersAreUnique();
		testSharedKeyRoundTrip();
		testKeyScopeMismatchFails();
		testVideoReorderAcrossFrameBoundary();
		testVideoFrameWithMissingPacketDropped();
		testPacketizedOriginDistinguishedFromSinglePacketFrame();
		testVideoPerPacketSFrame();
		testVideoMixedRunLengthsInOneTimestamp();
		testBadPacketsFiltered();
		testDescriptorByteEncoding();
		testBadDescriptorBitsIgnored();
		testAudioBadDescriptorIgnored();
		testHeaderExtensionsAccounted();
		testCsrcAccounted();
		testCsrcAndExtensionAccounted();
		testMultipleHeaderExtensionsAccounted();
		testPaddingStripped();
		testInvalidPaddingDropped();
		testUnsupportedModeRejected();
		testFailClosedOnMissingKeyMaterial();
		testSharedProviderAcrossTracks();
		testDerivedKeyCacheInvalidation();
		testControlMessagesPassThrough();
		testSFrameFramingNotStickyOnConfig();
		testMaxFragmentSizeHonoured();
		testSFrameNegotiationAttribute();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
#else
	return TestResult(true);
#endif
}
