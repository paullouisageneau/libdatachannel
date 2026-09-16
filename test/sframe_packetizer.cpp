/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframecodec.hpp"
#include "impl/sframeutility.hpp"
#include "rtc/rtc.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

namespace {

static const SSRC TEST_SSRC = 0x1234ABCD;
static const uint8_t TEST_PT = 96;
static const char *TEST_CNAME = "sframe-test";

// Independent literals so the static_asserts below fail if the library's on-wire descriptor
// bits change.
static constexpr uint8_t SFRAME_DESCRIPTOR_S = 0x80; // start
static constexpr uint8_t SFRAME_DESCRIPTOR_E = 0x40; // end
static constexpr uint8_t SFRAME_DESCRIPTOR_T = 0x20; // payload origin (0 raw, 1 packetized)
static_assert(impl::SFrameDescriptorS == SFRAME_DESCRIPTOR_S, "SFrame descriptor S bit changed");
static_assert(impl::SFrameDescriptorE == SFRAME_DESCRIPTOR_E, "SFrame descriptor E bit changed");
static_assert(impl::SFrameDescriptorT == SFRAME_DESCRIPTOR_T, "SFrame descriptor T value changed");

// RFC 9605-style key provider: it is provisioned only with the *session* (master) key and hands it
// back for every ratchet step of the generation it was registered under. The per-SSRC derivation belongs to the library, on both
// the send and the receive side, so the provider never sees an SSRC and keeps no
// per-call state -- which is what makes it safe to share across tracks.
// It also records every KID it was asked for, so tests can assert what the decoder looked up and
// how often.
class SessionKeyProvider final : public SFrameReceiveKeyProvider {
public:
	SessionKeyProvider(uint8_t cipherSuiteId, binary sessionKey, bool perSSRC = true,
	                   uint8_t ratchetStepBits = 0)
	    : SFrameReceiveKeyProvider(cipherSuiteId, ratchetStepBits, perSSRC) {
		addKey(/*kid=*/0, SFrameReceiveKey{std::move(sessionKey)});
	}

	optional<SFrameReceiveKey> receiveKey(uint64_t kid) const override {
		mLookups++;
		return SFrameReceiveKeyProvider::receiveKey(kid);
	}

	size_t lookups() const { return mLookups; }

	// Re-keys the one generation in place, modelling a provider that revokes material.
	void setKey(binary sessionKey) { addKey(/*kid=*/0, SFrameReceiveKey{std::move(sessionKey)}); }

private:
	mutable std::atomic<size_t> mLookups{0};
};

using MutableKeyProvider = SessionKeyProvider;

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
	auto config =
	    std::make_shared<RtpPacketizationConfig>(TEST_SSRC, TEST_CNAME, TEST_PT, clockRate);
	return config;
}

// Run the SFrame packetizer over a set of plaintext frames and collect the
// resulting RTP packets. Each frame gets a distinct RTP timestamp.
message_vector packetizeFramesWithSsrc(uint32_t clockRate, SSRC ssrc,
                                       const shared_ptr<SFrameSendKeyProvider> &keyProvider,
                                       const std::vector<binary> &frames) {
	auto config = std::make_shared<RtpPacketizationConfig>(ssrc, TEST_CNAME, TEST_PT, clockRate);
	SFramePerFrameRtpPacketizer packetizer(config, keyProvider);

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

message_vector packetizeFrames(uint32_t clockRate,
                               const shared_ptr<SFrameSendKeyProvider> &keyProvider,
                               const std::vector<binary> &frames) {
	return packetizeFramesWithSsrc(clockRate, TEST_SSRC, keyProvider, frames);
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

message_vector depacketizeVideo(message_vector packets,
                                shared_ptr<SFrameReceiveKeyProvider> provider) {
	SFramePerFrameVideoRtpDepacketizer depacketizer(std::move(provider));
	depacketizer.incoming(packets, [](message_ptr) {});
	return packets;
}

// One packet per incoming() call, which is what Track::incoming does on the wire. The batch
// form above lets several groups exist at once and so hides anything that only goes wrong
// when the end-of-batch flush runs after every packet.
message_vector depacketizeVideoPerPacket(message_vector packets,
                                         shared_ptr<SFrameReceiveKeyProvider> provider) {
	SFramePerFrameVideoRtpDepacketizer depacketizer(std::move(provider));
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
                                shared_ptr<SFrameReceiveKeyProvider> provider) {
	SFramePerFrameAudioRtpDepacketizer depacketizer(clockRate, std::move(provider));
	depacketizer.incoming(packets, [](message_ptr) {});
	return packets;
}

bool sameBytes(const binary &expected, const message_ptr &actual) {
	if (!actual || actual->size() != expected.size())
		return false;
	return std::equal(expected.begin(), expected.end(), actual->begin());
}

void check(bool condition, const string &message) {
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

// A send provider holding one key for the session, which is what most of these tests want.
shared_ptr<SFrameSendKeyProvider> makeSendProvider(uint8_t cipherSuiteId, const binary &sessionKey,
                                                   bool perSsrc = true, uint8_t ratchetStepBits = 0,
                                                   uint16_t ratchetPeriod = 0) {
	return std::make_shared<SFrameSendKeyProvider>(cipherSuiteId, ratchetStepBits, ratchetPeriod,
	                                               perSsrc, SFrameSendKey{sessionKey, /*kid=*/0});
}

// A padding-only RTP packet (RFC 3550 section 5.1): a header borrowed from `like`, the P bit
// set, and nothing but padding after it. A pacer emits these to probe bandwidth. They carry
// no media, so nothing can be reassembled from them, but they do consume a sequence number.
message_ptr makePaddingOnlyPacket(const message_ptr &like, uint32_t timestamp, uint16_t seq,
                                  uint8_t pad) {
	auto src = reinterpret_cast<const RtpHeader *>(like->data());
	binary out(like->begin(), like->begin() + src->getSize());
	auto header = reinterpret_cast<RtpHeader *>(out.data());
	header->setTimestamp(timestamp);
	header->setSeqNumber(seq);
	header->setMarker(false);
	out[0] = static_cast<std::byte>(static_cast<uint8_t>(out[0]) | 0x20); // P bit
	out.insert(out.end(), pad, std::byte{0x00});
	out.back() = static_cast<std::byte>(pad);
	return make_message(std::move(out), Message::Binary);
}

// Splice `count` padding-only packets in after index `after`, shifting every later packet's
// sequence number up so the stream stays contiguous on the wire, as it would be from a pacer.
// None of the spliced packets carries the marker bit.
message_vector withPaddingOnlyPacketsAfter(const message_vector &packets, size_t after,
                                           uint32_t paddingTimestamp, size_t count, uint8_t pad) {
	message_vector out;
	for (size_t i = 0; i < packets.size(); ++i) {
		auto header = reinterpret_cast<const RtpHeader *>(packets[i]->data());
		const uint16_t seq = uint16_t(header->seqNumber() + (i > after ? count : 0));
		out.push_back(withRtpFraming(packets[i], header->timestamp(), seq, header->marker() != 0));
		if (i != after)
			continue;
		for (size_t n = 0; n < count; ++n)
			out.push_back(makePaddingOnlyPacket(packets[i], paddingTimestamp,
			                                    uint16_t(header->seqNumber() + 1 + n), pad));
	}
	return out;
}

// A shared key with a ratchet field, for the shared-encoder path: no per-SSRC derivation, but
// the decoder still has to follow the ratchet chain from the base key.
shared_ptr<SFrameReceiveKeyProvider> ratchetingSharedProvider(uint8_t suite, const binary &key,
                                                              uint8_t ratchetStepBits) {
	auto provider = std::make_shared<SFrameReceiveKeyProvider>(suite, ratchetStepBits,
	                                                           /*perSsrcDerivation=*/false);
	provider->addKey(uint64_t(1) << ratchetStepBits, SFrameReceiveKey{key});
	return provider;
}

// Runs one frame through a packetizer and returns the RTP packets it produced.
message_vector packetizeWith(SFramePerFrameRtpPacketizer &packetizer, const binary &frame,
                             uint32_t timestamp) {
	auto frameInfo = std::make_shared<FrameInfo>(timestamp);
	frameInfo->payloadType = TEST_PT;
	message_vector messages;
	messages.push_back(make_message(binary(frame), frameInfo));
	packetizer.outgoing(messages, [](message_ptr) {});
	return messages;
}

// The KID from the SFrame header of a packet carrying the start of an object.
uint64_t kidOf(const message_ptr &packet) {
	auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
	const size_t hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
	binary sframe(packet->begin() + hdrSize + 1, packet->end());
	return impl::sframe::header::Decode(sframe).kid;
}

// The RTP payloads of one frame, descriptors stripped and concatenated: the SFrame packet as
// it appears on the wire.
binary wireBytes(const message_vector &packets) {
	binary out;
	for (const auto &packet : packets) {
		if (packet->type == Message::Control)
			continue;
		auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
		const size_t hdrSize = pkt->getSize() + pkt->getExtensionHeaderSize();
		out.insert(out.end(), packet->begin() + hdrSize + 1, packet->end());
	}
	return out;
}

// True if `needle` appears anywhere in `haystack`.
bool contains(const binary &haystack, const binary &needle) {
	if (needle.empty() || needle.size() > haystack.size())
		return false;
	return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
	       haystack.end();
}

// --- Individual scenarios -------------------------------------------------

// The 5 reserved descriptor bits are sent as zero but must be ignored on receive: a future
// extension that uses one has to stay decodable by this implementation, so rejecting a packet
// for setting one would be an interop failure rather than strictness. T=1 is different -- that
// is a mode this code declines, not a bit it does not understand.
void testReservedDescriptorBitsIgnored() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	const uint32_t audioClockRate = 48000;

	// One reserved bit, a middle one, and all five.
	for (uint8_t reserved : {uint8_t(0x01), uint8_t(0x04), uint8_t(0x1F)}) {
		// Video, single packet: S|E plus the reserved bits.
		{
			auto frame = makeFrame(200, 0x4A);
			auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
			                               makeSendProvider(suite, sessionKey), {frame});
			check(packets.size() == 1, "expected a single video packet");
			setDescriptorByte(packets.front(), uint8_t(0xC0 | reserved));

			auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
			auto decoded = depacketizeVideo(std::move(packets), provider);
			check(decoded.size() == 1, "video descriptor 0xC0|" + std::to_string(reserved) +
			                               " should still decode, got " +
			                               std::to_string(decoded.size()));
			check(sameBytes(frame, decoded.front()),
			      "video frame with reserved descriptor bits did not round-trip");
		}
		// Audio, single packet.
		{
			auto frame = makeFrame(160, 0x4B);
			auto packets =
			    packetizeFrames(audioClockRate, makeSendProvider(suite, sessionKey), {frame});
			check(packets.size() == 1, "expected a single audio packet");
			setDescriptorByte(packets.front(), uint8_t(0xC0 | reserved));

			auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
			auto decoded = depacketizeAudio(std::move(packets), audioClockRate, provider);
			check(decoded.size() == 1, "audio descriptor 0xC0|" + std::to_string(reserved) +
			                               " should still decode, got " +
			                               std::to_string(decoded.size()));
			check(sameBytes(frame, decoded.front()),
			      "audio frame with reserved descriptor bits did not round-trip");
		}
	}

	// And what we send still has them clear.
	{
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSendProvider(suite, sessionKey), {makeFrame(200, 0x4C)});
		check((descriptorByte(packets.front()) & 0x1F) == 0,
		      "the packetizer set a reserved descriptor bit");
	}
}

// A pacer may emit a padding-only packet (RFC 3550 section 5.1) between two packets of the same
// frame. It carries no SFrame object, so the sequence number it consumes must not read as a lost
// packet. Padding under a different RTP timestamp is the exception: the video path groups packets
// by timestamp, so it does count as loss there -- a documented limitation the codec depacketizers
// share -- while the audio path accumulates in arrival order and is unaffected either way.
void testPaddingOnlyPacketDoesNotBreakFrame() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(3000, 0x5A); // several packets
	const uint32_t audioClockRate = 48000;

	// `after` splices the padding inside the frame's packets; `count` padding packets of `pad`
	// bytes each; `sameTimestamp` chooses whether the padding carries the frame's timestamp.
	auto checkSplice = [&](size_t count, uint8_t pad, bool sameTimestamp) {
		const string label = "count=" + std::to_string(count) + " pad=" + std::to_string(pad) +
		                     (sameTimestamp ? " same timestamp" : " other timestamp");
		// Video groups by timestamp, so padding under another one reads as loss there; audio
		// does not group, so it survives regardless.
		const size_t expectedVideo = sameTimestamp ? 1 : 0;
		const size_t expectedAudio = 1;

		// Video, one packet per incoming() call, as Track::incoming delivers them.
		{
			auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
			                               makeSendProvider(suite, sessionKey), {frame});
			check(packets.size() >= 3,
			      "expected a multi-packet frame, got " + std::to_string(packets.size()));
			auto ts = reinterpret_cast<const RtpHeader *>(packets.front()->data())->timestamp();
			auto spliced =
			    withPaddingOnlyPacketsAfter(packets, 0, sameTimestamp ? ts : ts + 1500, count, pad);

			auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
			auto decoded = depacketizeVideoPerPacket(std::move(spliced), provider);
			check(decoded.size() == expectedVideo,
			      "video, " + label + ": expected " + std::to_string(expectedVideo) +
			          " frame(s), got " + std::to_string(decoded.size()));
			if (expectedVideo)
				check(sameBytes(frame, decoded.front()),
				      "video, " + label + ": frame did not round-trip");
		}
		// Audio, which reassembles in arrival order against its own expected sequence number.
		{
			auto packets =
			    packetizeFrames(audioClockRate, makeSendProvider(suite, sessionKey), {frame});
			check(packets.size() >= 3,
			      "expected a multi-packet audio frame, got " + std::to_string(packets.size()));
			auto ts = reinterpret_cast<const RtpHeader *>(packets.front()->data())->timestamp();
			auto spliced =
			    withPaddingOnlyPacketsAfter(packets, 0, sameTimestamp ? ts : ts + 1500, count, pad);

			auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
			auto decoded = depacketizeAudio(std::move(spliced), audioClockRate, provider);
			check(decoded.size() == expectedAudio,
			      "audio, " + label + ": expected " + std::to_string(expectedAudio) +
			          " frame(s), got " + std::to_string(decoded.size()));
			if (expectedAudio)
				check(sameBytes(frame, decoded.front()),
				      "audio, " + label + ": frame did not round-trip");
		}
	};

	// Padding length: 1 is the minimum, where the count byte is the only padding byte, and 255
	// is the maximum a one-byte count can express.
	for (uint8_t pad : {uint8_t(1), uint8_t(2), uint8_t(17), uint8_t(200), uint8_t(255)})
		checkSplice(/*count=*/1, pad, /*sameTimestamp=*/true);

	// A probe burst is several packets stepped over at once. Both timestamps, since padding need
	// not carry the frame's.
	for (size_t count : {size_t(1), size_t(2), size_t(8), size_t(32)})
		for (bool sameTimestamp : {true, false})
			checkSplice(count, /*pad=*/8, sameTimestamp);
}

// Everything else here compares plaintext in against plaintext out, which a packetizer that
// forgot to encrypt would also pass. This looks at the bytes actually put on the wire: the
// plaintext must not appear in them, and they must have grown by the SFrame header and tag.
void testWireBytesAreCiphertext() {
	const uint8_t suite = 0x04; // AES-128-GCM, 16-byte tag
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(1200, 0x6E);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSendProvider(suite, sessionKey), {frame});
	auto wire = wireBytes(packets);

	check(!contains(wire, frame), "the plaintext frame appears verbatim on the wire");

	// A 1-byte header (kid and ctr both below 8) plus the 16-byte tag.
	check(wire.size() == frame.size() + 1 + 16, "expected " + std::to_string(frame.size() + 17) +
	                                                " wire bytes, got " +
	                                                std::to_string(wire.size()));

	// Nor should any recognisable run of it survive: the frame is a counter pattern, so a
	// cipher that leaked structure would show up as a matching 32-byte window.
	binary window(frame.begin(), frame.begin() + 32);
	check(!contains(wire, window), "a 32-byte run of plaintext appears on the wire");

	// And it still decrypts, so the bytes are ciphertext rather than corruption.
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.size() == 1 && sameBytes(frame, decoded.front()),
	      "the encrypted frame did not round-trip");
}

// The SFrame header is 1 to 17 bytes: a config byte plus up to 8 each of KID and CTR (RFC 9605
// Section 4.3). Fragmentation happens after encryption, so the header is inside what gets
// chunked and cannot push a chunk over the limit -- but that holds by construction rather than
// by arithmetic, so it is worth pinning at the largest header the format allows.
void testMaxHeaderStaysUnderMtu() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	const size_t maxFragmentSize = 101;

	// Both fields need all eight bytes at or above 2^56.
	const uint64_t fullKid = 0xFFFFFFFFFFFFFFFFull;
	const uint64_t fullCtr = uint64_t(1) << 56;

	auto sendKeys = std::make_shared<SFrameSendKeyProvider>(
	    suite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
	    SFrameSendKey{sessionKey, fullKid, fullCtr});

	auto rtpConfig = makeConfig(RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizer(rtpConfig, sendKeys, maxFragmentSize);

	auto frame = makeFrame(1500, 0x5E);
	auto messages = packetizeWith(packetizer, frame, 1000);
	check(messages.size() > 1, "expected the frame to fragment");

	// The header really is at its maximum, or the rest of this proves nothing.
	auto wire = wireBytes(messages);
	const size_t headerSize = impl::sframe::header::Decode(wire).length;
	check(headerSize == 17, "expected a 17-byte header, got " + std::to_string(headerSize));

	for (const auto &packet : messages) {
		auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
		const size_t payload = packet->size() - (pkt->getSize() + pkt->getExtensionHeaderSize());
		check(payload <= maxFragmentSize, "payload of " + std::to_string(payload) +
		                                      " exceeds the " + std::to_string(maxFragmentSize) +
		                                      " byte limit with a full-size header");
	}

	auto provider = std::make_shared<SFrameReceiveKeyProvider>(suite, /*ratchetStepBits=*/0,
	                                                           /*perSsrcDerivation=*/true);
	provider->addKey(fullKid, SFrameReceiveKey{sessionKey});
	auto decoded = depacketizeVideo(std::move(messages), provider);
	check(decoded.size() == 1 && sameBytes(frame, decoded.front()),
	      "a frame with a full-size header did not round-trip");
}

// The header grows mid-stream: a CTR of 0-7 fits in the config byte, 8 needs an extra byte. So
// the same packetizer sending the same frame size produces a longer packet from the ninth frame
// on, and the chunk budget has to absorb that rather than overflow.
void testHeaderGrowthMidStreamStaysUnderMtu() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	const size_t maxFragmentSize = 101;

	auto sendKeys = std::make_shared<SFrameSendKeyProvider>(
	    suite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
	    SFrameSendKey{sessionKey, /*kid=*/1, /*ctrStart=*/0});

	auto rtpConfig = makeConfig(RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizer(rtpConfig, sendKeys, maxFragmentSize);

	auto provider = std::make_shared<SFrameReceiveKeyProvider>(suite, /*ratchetStepBits=*/0,
	                                                           /*perSsrcDerivation=*/true);
	provider->addKey(/*kid=*/1, SFrameReceiveKey{sessionKey});

	std::set<size_t> headerSizes;
	uint32_t timestamp = 1000;

	// Across the 7 -> 8 boundary and well past it.
	for (int i = 0; i < 12; ++i) {
		auto frame = makeFrame(1500, static_cast<uint8_t>(0x60 + i));
		auto messages = packetizeWith(packetizer, frame, timestamp);
		timestamp += 3000;

		headerSizes.insert(impl::sframe::header::Decode(wireBytes(messages)).length);

		for (const auto &packet : messages) {
			auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
			const size_t payload =
			    packet->size() - (pkt->getSize() + pkt->getExtensionHeaderSize());
			check(payload <= maxFragmentSize, "frame " + std::to_string(i) + ": payload of " +
			                                      std::to_string(payload) + " exceeds the " +
			                                      std::to_string(maxFragmentSize) + " byte limit");
		}

		auto decoded = depacketizeVideo(std::move(messages), provider);
		check(decoded.size() == 1 && sameBytes(frame, decoded.front()),
		      "frame " + std::to_string(i) + " did not round-trip");
	}

	// The header did grow, or this tested nothing: 1 byte while the CTR is under 8, 2 after.
	check(headerSizes.size() > 1,
	      "the header never grew across the CTR boundary, so the case was not exercised");
}

// Fragmentation is driven by maxFragmentSize, and the descriptor byte comes out of that budget, so
// the chunk payload is one byte smaller. Sizes are chosen to land exactly on a chunk boundary and
// one byte either side of it, where an off-by-one shows up as a spurious extra packet or an
// over-long one.
void testMaxFragmentSizeBoundary() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	const size_t maxFragmentSize = 101;
	const size_t chunkSize = maxFragmentSize - 1; // the descriptor takes one byte
	// A fresh packetizer starts at CTR 0 with a small KID, so the header is one byte. The
	// extended-header cases are testHeaderGrowthMidStreamStaysUnderMtu and
	// testMaxHeaderStaysUnderMtu above.
	const size_t overhead = 1 + 16; // 1-byte SFrame header, 16-byte GCM tag

	// ciphertext = plaintext + overhead, so this lands the ciphertext on exactly 3 chunks.
	for (auto [plaintextSize, expectedPackets] :
	     std::vector<std::pair<size_t, size_t>>{{3 * chunkSize - overhead - 1, 3},
	                                            {3 * chunkSize - overhead, 3},
	                                            {3 * chunkSize - overhead + 1, 4}}) {
		auto frame = makeFrame(plaintextSize, 0x71);
		auto rtpConfig = makeConfig(RtpPacketizer::VideoClockRate);
		SFramePerFrameRtpPacketizer packetizer(rtpConfig, makeSendProvider(suite, sessionKey),
		                                       maxFragmentSize);

		auto frameInfo = std::make_shared<FrameInfo>(1000);
		frameInfo->payloadType = TEST_PT;
		message_vector messages;
		messages.push_back(make_message(binary(frame), frameInfo));
		packetizer.outgoing(messages, [](message_ptr) {});

		const string label =
		    std::to_string(plaintextSize) + " byte frame at MTU " + std::to_string(maxFragmentSize);
		check(messages.size() == expectedPackets,
		      label + ": expected " + std::to_string(expectedPackets) + " packets, got " +
		          std::to_string(messages.size()));

		for (const auto &packet : messages) {
			auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
			const size_t payload =
			    packet->size() - (pkt->getSize() + pkt->getExtensionHeaderSize());
			check(payload <= maxFragmentSize,
			      label + ": payload of " + std::to_string(payload) + " exceeds the limit");
		}

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(messages, provider);
		check(decoded.size() == 1 && sameBytes(frame, decoded.front()),
		      label + ": frame did not round-trip");
	}
}

// The chunk size is clamped, not trusted: the descriptor byte comes out of maxFragmentSize, so a
// limit of 0 or 1 leaves nothing for payload. The clamp keeps one payload byte per packet, which
// means the whole ciphertext still reaches the wire and the loop still terminates. A
// "simplification" that dropped the clamp would either spin on a zero-length chunk or emit
// descriptor-only packets carrying no object. VP9's packetizer returns {} for the same input; this
// one deliberately does not, so the behaviour is pinned here rather than assumed to match.
void testMaxFragmentSizeClampedToOneBytePayload() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(20, 0x9C);
	// A fresh packetizer starts at CTR 0 with a small KID, so the header is one byte.
	const size_t ciphertextSize = frame.size() + 1 + 16; // 1-byte SFrame header, 16-byte GCM tag

	// 0 and 1 are clamped up to a 1-byte chunk and 2 already is one, so all three must agree.
	for (size_t maxFragmentSize : {size_t(0), size_t(1), size_t(2)}) {
		const string label = "maxFragmentSize=" + std::to_string(maxFragmentSize);

		auto rtpConfig = makeConfig(RtpPacketizer::VideoClockRate);
		SFramePerFrameRtpPacketizer packetizer(rtpConfig, makeSendProvider(suite, sessionKey),
		                                       maxFragmentSize);
		auto messages = packetizeWith(packetizer, frame, 1000);

		check(messages.size() == ciphertextSize,
		      label + ": expected one packet per ciphertext byte (" +
		          std::to_string(ciphertextSize) + "), got " + std::to_string(messages.size()));
		checkFrameMarkers(messages);

		for (const auto &packet : messages) {
			auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
			const size_t payload =
			    packet->size() - (pkt->getSize() + pkt->getExtensionHeaderSize());
			check(payload == 2, label + ": expected a descriptor plus one payload byte, got " +
			                        std::to_string(payload));
		}

		// Nothing was dropped on the way: the wire bytes add up to the whole ciphertext, and it
		// still decrypts a byte at a time.
		check(wireBytes(messages).size() == ciphertextSize,
		      label + ": the wire bytes do not add up to the ciphertext, got " +
		          std::to_string(wireBytes(messages).size()));

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(messages, provider);
		check(decoded.size() == 1 && sameBytes(frame, decoded.front()),
		      label + ": the frame did not round-trip one byte per packet");
	}
}

// Shared keying and ratcheting together. Two tracks share one encoder, so they share one
// counter and one ratchet: when the period elapses both move to the next KID at the same
// time, and each receiver has to follow the chain without per-SSRC derivation. Tested
// together because the shared-encoder path builds the derived key once for both tracks, which is
// exactly where a ratchet could be applied to one and not the other.
void testSharedKeyRatchets() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	const uint8_t ratchetStepBits = 4;

	auto sendProvider = std::make_shared<SFrameSendKeyProvider>(
	    suite, ratchetStepBits, /*ratchetPeriod=*/1, /*perSsrcDerivation=*/false,
	    SFrameSendKey{sessionKey, uint64_t(1) << ratchetStepBits});
	auto videoRtp = makeConfig(RtpPacketizer::VideoClockRate);
	auto audioRtp = makeConfig(48000);
	SFramePerFrameRtpPacketizer videoPacketizer(videoRtp, sendProvider);
	SFramePerFrameRtpPacketizer audioPacketizer(audioRtp, sendProvider);

	auto videoProvider = ratchetingSharedProvider(suite, sessionKey, ratchetStepBits);
	auto audioProvider = ratchetingSharedProvider(suite, sessionKey, ratchetStepBits);
	SFramePerFrameVideoRtpDepacketizer videoDepacketizer(videoProvider);
	SFramePerFrameAudioRtpDepacketizer audioDepacketizer(48000, audioProvider);

	std::set<uint64_t> videoKids, audioKids;
	uint32_t timestamp = 1000;

	// Long enough to cross the one-second period, so both tracks must ratchet.
	for (int i = 0; i < 6; ++i) {
		auto videoFrame = makeFrame(600, static_cast<uint8_t>(0x80 + i));
		auto audioFrame = makeFrame(200, static_cast<uint8_t>(0xC0 + i));

		auto videoPackets = packetizeWith(videoPacketizer, videoFrame, timestamp);
		auto audioPackets = packetizeWith(audioPacketizer, audioFrame, timestamp);
		for (const auto &packet : videoPackets)
			videoKids.insert(kidOf(packet));
		for (const auto &packet : audioPackets)
			audioKids.insert(kidOf(packet));

		videoDepacketizer.incoming(videoPackets, [](message_ptr) {});
		audioDepacketizer.incoming(audioPackets, [](message_ptr) {});

		check(videoPackets.size() == 1 && sameBytes(videoFrame, videoPackets.front()),
		      "video frame " + std::to_string(i) + " did not round-trip under a shared ratchet");
		check(audioPackets.size() == 1 && sameBytes(audioFrame, audioPackets.front()),
		      "audio frame " + std::to_string(i) + " did not round-trip under a shared ratchet");

		timestamp += 3000;
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	check(videoKids.size() > 1, "the shared encoder never ratcheted on the video track");
	check(audioKids.size() > 1, "the shared encoder never ratcheted on the audio track");
	check(videoKids == audioKids,
	      "the two tracks sharing one encoder used different KIDs, so the ratchet was applied "
	      "to one and not the other");
}

// Shared keying means one encoder for every track, so one rotation has to move all of them.
// Rekeying per packetizer instead could leave tracks straddling two generations on one
// counter, which is the hazard the shared-encoder constructor exists to prevent.
void testSharedEncoderRekeyMovesAllTracks() {
	const uint8_t suite = 0x04;
	auto firstKey = makeSessionKey(suite);
	binary secondKey(firstKey.size(), std::byte{0x7B});

	auto provider = std::make_shared<SFrameSendKeyProvider>(
	    suite, /*ratchetStepBits=*/4, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
	    SFrameSendKey{firstKey, uint64_t(1) << 4});
	SFramePerFrameRtpPacketizer videoPacketizer(makeConfig(RtpPacketizer::VideoClockRate),
	                                            provider);
	SFramePerFrameRtpPacketizer audioPacketizer(makeConfig(48000), provider);

	auto videoBefore = packetizeWith(videoPacketizer, makeFrame(400, 0x90), 1000);
	auto audioBefore = packetizeWith(audioPacketizer, makeFrame(160, 0x91), 1000);

	provider->rollKey(SFrameSendKey{secondKey, uint64_t(2) << 4});

	auto videoAfter = packetizeWith(videoPacketizer, makeFrame(400, 0x92), 4000);
	auto audioAfter = packetizeWith(audioPacketizer, makeFrame(160, 0x93), 4000);

	auto generationOf = [](const message_ptr &packet) {
		return impl::sframe::KeyGenerationFromKid(kidOf(packet), 4);
	};

	check(generationOf(videoBefore.front()) == 1 && generationOf(audioBefore.front()) == 1,
	      "tracks did not start on the first key generation");
	check(generationOf(videoAfter.front()) == 2,
	      "the video track did not follow the shared encoder's rekey");
	check(generationOf(audioAfter.front()) == 2,
	      "the audio track did not follow the shared encoder's rekey");
}

// The two directions cannot be crossed: an SFrameSendKey comes only from an
// SFrameSendKeyProvider and an SFrameReceiveKey only from an SFrameReceiveKeyProvider, so RFC
// 9605 Section 4.4.1's "never both" is a compile error rather than a runtime check. Likewise
// perSsrcDerivation is a constructor argument, so it cannot be left unset. Nothing to test at
// runtime for either.

// With derivation off the base key is used as-is, so a receiver whose provider also answers
// false decodes -- and one that answers true does not.
void testPerSsrcDerivationOffRoundTrips() {
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x3C);

	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/false);
	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate, sendProvider, {frame});

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
	                               makeSendProvider(suite, sessionKey), {frame});
	check(packets.size() > 1, "expected a fragmented frame for this test");

	auto firstHeader = reinterpret_cast<const RtpHeader *>(packets.front()->data());
	const uint32_t timestamp = firstHeader->timestamp();
	const uint16_t seq = firstHeader->seqNumber();

	// Opens a run that never closes, so the group can never resolve on its own.
	auto poison = withRtpFraming(packets.front(), timestamp + 0x40000000u,
	                             static_cast<uint16_t>(seq + 1000), /*marker=*/false);
	setDescriptorByte(poison, impl::SFrameDescriptorS);

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
	                               makeSendProvider(suite, sessionKey), {frame});
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
	                               makeSendProvider(suite, sessionKey), {frame});
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
	std::vector<binary> frames = {makeFrame(150, 0x01), makeFrame(2500, 0x02), makeFrame(300, 0x03),
	                              makeFrame(4096, 0x04)};

	auto packets =
	    packetizeFrames(RtpPacketizer::VideoClockRate, makeSendProvider(suite, sessionKey), frames);

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
	// Every suite the implementation knows from the IANA SFrame registry
	// (https://www.iana.org/assignments/sframe)
	for (uint8_t suite : {uint8_t(0x01), uint8_t(0x02), uint8_t(0x03), uint8_t(0x04), uint8_t(0x05),
	                      uint8_t(0x06), uint8_t(0x07), uint8_t(0x08)}) {
		auto sessionKey = makeSessionKey(suite);
		auto frame = makeFrame(1800, static_cast<uint8_t>(0x30 + suite));

		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSendProvider(suite, sessionKey), {frame});
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
	                               makeSendProvider(suite, sessionKey), {goodFrame, badFrame});

	// The good frame uses RTP timestamp 1000, the second frame 4000. Corrupt
	// the last std::byte (inside the auth tag) of the second frame's packet.
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
// through SFramePerFrameAudioRtpDepacketizer at an audio clock rate.
void testAudioFrameRoundTrip() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000; // Opus
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(160, 0x77);

	auto packets = packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {frame});
	check(packets.size() == 1,
	      "audio frame should produce one RTP packet, got " + std::to_string(packets.size()));
	checkFrameMarkers(packets);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeAudio(packets, clockRate, provider);
	check(decoded.size() == 1,
	      "expected 1 decrypted audio frame, got " + std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "audio frame did not round-trip");
}

// A frame timed in seconds is converted to RTP ticks with the clock rate of the config the
// packetizer was built with. Nothing on the wire carries the rate, so a packetizer that used a
// default instead would stamp an Opus track as if it were 90 kHz video and the receiver's playout
// would be wrong by the ratio of the two. The whole frame is one instant, so every fragment of it
// carries that one timestamp.
void testClockRateHonouredInRtpTimestamps() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	const double seconds = 0.5;

	std::set<uint32_t> ticks;
	// G.711, Opus and the video rate: the same capture instant is a different tick count in each.
	for (uint32_t clockRate : {uint32_t(8000), uint32_t(48000), RtpPacketizer::VideoClockRate}) {
		auto config = makeConfig(clockRate);
		const uint32_t startTimestamp = config->startTimestamp;
		SFramePerFrameRtpPacketizer packetizer(config, makeSendProvider(suite, sessionKey));

		// Timed in seconds rather than in ticks, so the conversion is the packetizer's to make.
		auto frameInfo = std::make_shared<FrameInfo>(std::chrono::duration<double>(seconds));
		frameInfo->payloadType = TEST_PT;
		message_vector messages{make_message(makeFrame(3000, 0x9A), frameInfo)};
		packetizer.outgoing(messages, [](message_ptr) {});
		check(messages.size() > 1, "expected the frame to fragment, got " +
		                               std::to_string(messages.size()) + " packet(s)");

		const uint32_t expected = uint32_t(std::llround(seconds * double(clockRate)));
		const string label = "clock rate " + std::to_string(clockRate);
		for (const auto &packet : messages) {
			auto header = reinterpret_cast<const RtpHeader *>(packet->data());
			const uint32_t elapsed = header->timestamp() - startTimestamp;
			check(elapsed == expected, label + ": expected " + std::to_string(expected) +
			                               " ticks for " + std::to_string(seconds) + "s, got " +
			                               std::to_string(elapsed));
		}
		ticks.insert(expected);
	}

	// Or the loop above compared three identical numbers and pinned nothing.
	check(ticks.size() == 3, "the three clock rates should give three different tick counts");
}

// The audio depacketizer converts the RTP timestamp to a presentation time with the clock rate it
// was constructed with: the wire carries ticks only, so nothing else can supply the rate. The same
// packets therefore decode to a different presentation time under a different rate, while the
// plaintext is unaffected -- the clock rate is no part of the SFrame nonce or AAD, so a mismatched
// rate is a silent timing error rather than a decryption failure.
void testAudioDepacketizerUsesConstructedClockRate() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(160, 0x9B);

	auto packets = packetizeFrames(48000, makeSendProvider(suite, sessionKey), {frame});
	check(packets.size() == 1, "expected a single audio packet");
	const uint32_t timestamp =
	    reinterpret_cast<const RtpHeader *>(packets.front()->data())->timestamp();

	auto checkPresentationTime = [](const message_ptr &decoded, uint32_t timestamp,
	                                uint32_t clockRate, const string &label) {
		const auto &frameInfo = decoded->frameInfo;
		check(frameInfo != nullptr, label + ": the decrypted frame carries no FrameInfo");
		check(frameInfo->timestamp == timestamp,
		      label + ": the RTP timestamp was not carried through, got " +
		          std::to_string(frameInfo->timestamp));
		check(frameInfo->timestampSeconds.has_value(),
		      label + ": the presentation time was left unset");
		const double expected = double(timestamp) / double(clockRate);
		check(std::fabs(frameInfo->timestampSeconds->count() - expected) < 1e-6,
		      label + ": expected " + std::to_string(expected) + "s for timestamp " +
		          std::to_string(timestamp) + ", got " +
		          std::to_string(frameInfo->timestampSeconds->count()) + "s");
	};

	// The one set of packets decoded at three rates, including two the sender did not use.
	for (uint32_t clockRate : {uint32_t(8000), uint32_t(16000), uint32_t(48000)}) {
		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		const string label = "audio at " + std::to_string(clockRate);
		check(decoded.size() == 1,
		      label + ": expected 1 frame, got " + std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()), label + ": frame did not round-trip");
		checkPresentationTime(decoded.front(), timestamp, clockRate, label);
	}

	// Video has no such parameter: its rate is fixed at 90 kHz by RTP convention, and the
	// depacketizer must use that rather than whatever the audio side was given.
	{
		auto videoPackets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                                    makeSendProvider(suite, sessionKey), {frame});
		check(videoPackets.size() == 1, "expected a single video packet");
		const uint32_t videoTimestamp =
		    reinterpret_cast<const RtpHeader *>(videoPackets.front()->data())->timestamp();

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(std::move(videoPackets), provider);
		check(decoded.size() == 1,
		      "video: expected 1 frame, got " + std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()), "video: frame did not round-trip");
		checkPresentationTime(decoded.front(), videoTimestamp, RtpPacketizer::VideoClockRate,
		                      "video");
	}
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
		auto packets = packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {large});
		check(packets.size() > 1, "a 2000 byte audio frame should fragment, got " +
		                              std::to_string(packets.size()) + " packet(s)");

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1,
		      "expected 1 reassembled audio frame, got " + std::to_string(decoded.size()));
		check(sameBytes(large, decoded.front()), "large audio frame did not round-trip");
	}

	// Small and large interleaved, all delivered and in order
	{
		auto packets =
		    packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {small, large, small});
		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 3,
		      "expected 3 audio frames, got " + std::to_string(decoded.size()));
		check(sameBytes(small, decoded[0]), "first small frame did not round-trip");
		check(sameBytes(large, decoded[1]), "large frame did not round-trip");
		check(sameBytes(small, decoded[2]), "second small frame did not round-trip");
	}

	// Losing the S packet must drop only that frame, not poison the next one
	{
		auto packets =
		    packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {large, small});
		check(packets.size() > 2, "expected the large frame to fragment");
		packets.erase(packets.begin()); // drop the S packet of the large frame

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1,
		      "expected only the small frame to survive, got " + std::to_string(decoded.size()));
		check(sameBytes(small, decoded.front()), "the following frame did not round-trip");
	}

	// Losing the E packet must drop that frame and still deliver the next
	{
		auto packets =
		    packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {large, small});
		// The large frame's last packet is the one before the small frame's single packet
		check(packets.size() > 2, "expected the large frame to fragment");
		packets.erase(packets.end() - 2);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1,
		      "expected only the small frame to survive, got " + std::to_string(decoded.size()));
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
		auto packets =
		    packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {large, next});
		check(packets.size() >= 5, "expected the large frame to span at least 4 packets, got " +
		                               std::to_string(packets.size()));
		std::swap(packets[1], packets[2]);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1,
		      "expected only the following frame, got " + std::to_string(decoded.size()));
		check(sameBytes(next, decoded.front()), "the following frame did not round-trip");
		check(provider->lookups() == 1,
		      "expected 1 key lookup for the intact frame, got " +
		          std::to_string(provider->lookups()) +
		          ": the reordered frame was spliced and sent to decryption instead of "
		          "being rejected from its sequence numbers");
	}

	// A lost middle fragment leaves a gap
	{
		auto packets =
		    packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {large, next});
		packets.erase(packets.begin() + 1);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeAudio(packets, clockRate, provider);
		check(decoded.size() == 1,
		      "expected only the following frame, got " + std::to_string(decoded.size()));
		check(sameBytes(next, decoded.front()), "the following frame did not round-trip");
		check(provider->lookups() == 1, "expected 1 key lookup for the intact frame, got " +
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
	                               makeSendProvider(suite, sessionKey), {frameA, frameB});
	check(packets.size() == 4,
	      "expected 2 packets per frame, got " + std::to_string(packets.size()));

	// a(S) c(S) b(E) d(E): the second frame's first packet overtakes the first frame's last
	message_vector reordered = {packets[0], packets[2], packets[1], packets[3]};

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(reordered, provider);
	check(decoded.size() == 2,
	      "expected both frames to survive the reorder, got " + std::to_string(decoded.size()));
	check(sameBytes(frameA, decoded[0]), "first frame did not round-trip after reorder");
	check(sameBytes(frameB, decoded[1]), "second frame did not round-trip after reorder");
}

// An injected object that assembles but never authenticates must not move the staleness
// watermark. One packet stamped far in the future would otherwise make every genuine frame
// behind it look like a late straggler, for hours of media time.
void testUnauthenticatedObjectDoesNotSetWatermark() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	std::vector<binary> frames{makeFrame(400, 0x71), makeFrame(400, 0x72), makeFrame(400, 0x73)};

	auto packets =
	    packetizeFrames(RtpPacketizer::VideoClockRate, makeSendProvider(suite, sessionKey), frames);

	// A whole object (S and E, one payload byte) the receiver can assemble but never decrypt,
	// stamped as far ahead as the signed comparison can reach.
	auto forged = make_message(binary(packets.front()->begin(), packets.front()->end()));
	auto header = reinterpret_cast<RtpHeader *>(forged->data());
	header->setTimestamp(header->timestamp() + 0x7FFFFFFF);
	header->setMarker(true);
	setDescriptorByte(forged, impl::SFrameDescriptorS | impl::SFrameDescriptorE);
	const size_t hdrSize = header->getSize() + header->getExtensionHeaderSize();
	forged->resize(hdrSize + 2);

	message_vector input{forged};
	for (auto &p : packets)
		input.push_back(p);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideoPerPacket(std::move(input), provider);
	check(decoded.size() == frames.size(),
	      "expected " + std::to_string(frames.size()) +
	          " frames to survive an injected unauthenticated object, got " +
	          std::to_string(decoded.size()));
	for (size_t i = 0; i < frames.size(); ++i)
		check(sameBytes(frames[i], decoded[i]),
		      "frame " + std::to_string(i) + " did not round-trip after the injected object");
}

// A frame missing a middle packet must be dropped, not concatenated spliced, and it must
// not block the frames behind it.
void testVideoFrameWithMissingPacketDropped() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto big = makeFrame(3600, 0x51); // three packets
	auto next = makeFrame(400, 0x52); // one packet

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSendProvider(suite, sessionKey), {big, next});
	check(packets.size() >= 4, "expected the large frame to span several packets");

	// Drop the middle packet of the first frame
	message_vector damaged;
	for (size_t i = 0; i < packets.size(); ++i)
		if (i != 1)
			damaged.push_back(packets[i]);

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(damaged, provider);
	check(decoded.size() == 1,
	      "expected only the intact frame, got " + std::to_string(decoded.size()));
	check(sameBytes(next, decoded.front()), "the frame after the damaged one did not round-trip");

	// The gap is visible in the RTP sequence numbers, so the frame should be rejected
	// before any key material is derived. Authentication would also reject the spliced
	// ciphertext, but only after a wasted HKDF derivation and decrypt -- and the key
	// lookup is the observable difference between the two.
	check(provider->lookups() == 1,
	      "expected 1 key lookup for the intact frame, got " + std::to_string(provider->lookups()) +
	          ": the damaged frame reached key derivation instead of being rejected "
	          "from its sequence numbers");
}

// Several whole SFrame objects sharing one RTP timestamp, each in a single packet, so every
// descriptor sets both S and E. Still per-frame origin (T=0) -- per-packet mode is T=1 and is
// declined. draft-ietf-avtcore-rtp-sframe section 5.2 delimits objects by the descriptor bits
// rather than by timestamp, so every object must be decrypted and delivered.
void testVideoPerPacketSFrame() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	// Three small objects, one packet each, every one its own SFrame ciphertext with its
	// own counter.
	std::vector<binary> objects = {makeFrame(200, 0x61), makeFrame(200, 0x62),
	                               makeFrame(200, 0x63)};
	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSendProvider(suite, sessionKey), objects);
	check(packets.size() == 3,
	      "expected one packet per object, got " + std::to_string(packets.size()));

	// Collapse them onto one RTP timestamp with consecutive sequence numbers, the marker
	// only on the last: one video frame carried as three per-packet SFrame objects.
	message_vector perPacket;
	for (size_t i = 0; i < packets.size(); ++i) {
		check(descriptorByte(packets[i]) == uint8_t(SFRAME_DESCRIPTOR_S | SFRAME_DESCRIPTOR_E),
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
	                               makeSendProvider(suite, sessionKey), {big, small});
	check(packets.size() == 3, "expected 2 packets then 1, got " + std::to_string(packets.size()));

	message_vector merged;
	for (size_t i = 0; i < packets.size(); ++i)
		merged.push_back(
		    withRtpFraming(packets[i], 12000, uint16_t(500 + i), i + 1 == packets.size()));

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(merged, provider);
	check(decoded.size() == 2, "expected 2 objects from the mixed timestamp group, got " +
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
		auto packets = packetizeFrames(48000, makeSendProvider(suite, sessionKey), {frame});
		check(packets.size() == 1, "expected a single audio packet");
		return packets;
	};

	// T=0: an ordinary per-frame object that happens to fit one packet.
	{
		auto packets = build();
		check(descriptorByte(packets.front()) == uint8_t(SFRAME_DESCRIPTOR_S | SFRAME_DESCRIPTOR_E),
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
		setDescriptorByte(packets.front(),
		                  uint8_t(SFRAME_DESCRIPTOR_S | SFRAME_DESCRIPTOR_E | SFRAME_DESCRIPTOR_T));

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

		const string what = "RTCP type " + std::to_string(packetType);

		{
			message_vector batch = {make_message(rtcp.begin(), rtcp.end(), Message::Control)};
			SFramePerFrameVideoRtpDepacketizer d(
			    std::make_shared<SessionKeyProvider>(suite, sessionKey));
			d.incoming(batch, [](message_ptr) {});
			check(batch.size() == 1, what + " was dropped by the video depacketizer");
			check(batch.front()->type == Message::Control,
			      what + " lost its Control type in the video depacketizer");
			check(binary(batch.front()->begin(), batch.front()->end()) == rtcp,
			      what + " was modified by the video path");
		}

		{
			message_vector batch = {make_message(rtcp.begin(), rtcp.end(), Message::Control)};
			SFramePerFrameAudioRtpDepacketizer d(
			    48000, std::make_shared<SessionKeyProvider>(suite, sessionKey));
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
		                               makeSendProvider(suite, sessionKey), {frame});
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
		check(control == 1,
		      "expected the RTCP packet alongside the media, got " + std::to_string(control));
		check(media == 1, "expected 1 decrypted frame, got " + std::to_string(media));
	}
}

// Shared keying (no per-SSRC derivation) means every track sends under one key, so the
// counter has to be shared too: two tracks each starting at ctrStart would emit the same
// (key, CTR) and so the same nonce, which on the GCM suites leaks the GHASH key. The provider owns
// the one encoder every track uses, and this pins that the counters really are one sequence.
void testSharedKeyCountersAreUnique() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	auto sendProvider = std::make_shared<SFrameSendKeyProvider>(
	    suite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
	    SFrameSendKey{sessionKey, /*kid=*/1});
	// Two tracks, different SSRCs, one encoder.
	auto videoConfig = makeConfig(RtpPacketizer::VideoClockRate);
	videoConfig->ssrc = 1111;
	auto audioConfig = makeConfig(48000);
	audioConfig->ssrc = 2222;

	SFramePerFrameRtpPacketizer videoPacketizer(videoConfig, sendProvider);
	SFramePerFrameRtpPacketizer audioPacketizer(audioConfig, sendProvider);

	std::set<uint64_t> counters;
	const size_t framesPerTrack = 20;
	for (size_t i = 0; i < framesPerTrack; ++i) {
		for (auto *packetizer : {&videoPacketizer, &audioPacketizer}) {
			auto frameInfo = std::make_shared<FrameInfo>(uint32_t(3000 * (i + 1)));
			frameInfo->payloadType = TEST_PT;
			message_vector messages = {make_message(makeFrame(200, uint8_t(i)), frameInfo)};
			packetizer->outgoing(messages, [](message_ptr) {});

			for (const auto &m : messages) {
				size_t hdrSize = 0, payloadEnd = 0;
				if (!impl::sframe::ParseSFramePacket(m, hdrSize, payloadEnd))
					continue;
				// Skip the descriptor byte to reach the SFrame header.
				binary object(m->begin() + hdrSize + 1, m->begin() + payloadEnd);
				if (object.empty())
					continue;
				counters.insert(impl::sframe::header::Decode(object).ctr);
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
shared_ptr<SFrameReceiveKeyProvider> sharedKeyProvider(uint8_t suite, const binary &key) {
	auto provider = std::make_shared<SFrameReceiveKeyProvider>(suite, /*ratchetStepBits=*/0,
	                                                           /*perSsrcDerivation=*/false);
	provider->addKey(/*kid=*/1, SFrameReceiveKey{key});
	return provider;
}

void testSharedKeyRoundTrip() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	auto sendProvider = std::make_shared<SFrameSendKeyProvider>(
	    suite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
	    SFrameSendKey{sessionKey, /*kid=*/1});
	auto rtpConfig = makeConfig(RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizer(rtpConfig, sendProvider);

	auto frame = makeFrame(1500, 0xA7); // large enough to fragment
	auto frameInfo = std::make_shared<FrameInfo>(3000);
	frameInfo->payloadType = TEST_PT;
	message_vector packets = {make_message(binary(frame), frameInfo)};
	packetizer.outgoing(packets, [](message_ptr) {});
	check(packets.size() > 1, "expected the frame to fragment");

	auto provider = sharedKeyProvider(suite, sessionKey);
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
	                               makeSendProvider(suite, sessionKey), {makeFrame(400, 0xB8)});

	// Receiver does not, so it derives the base key instead of the per-SSRC one.
	auto provider = sharedKeyProvider(suite, sessionKey);
	auto decoded = depacketizeVideo(packets, provider);
	check(decoded.empty(),
	      "a frame decoded even though the sender derived per SSRC and the receiver did not");
}

// Malformed RTP packets pushed through the depacketizer must be filtered out
// by the length checks (too small for an RTP header + descriptor; header with
// no payload beyond the descriptor) without crashing or corrupting a valid
// frame received alongside them.
void testBadPacketsFiltered() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto frame = makeFrame(200, 0x5A);

	auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
	                               makeSendProvider(suite, sessionKey), {frame});
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
// the descriptor encoding in fragment(). The bits are determined by the
// fragment's position and are independent of encryption.
void testDescriptorByteEncoding() {
	const uint8_t S = SFRAME_DESCRIPTOR_S;
	const uint8_t E = SFRAME_DESCRIPTOR_E;
	const uint8_t suite = 0x01;
	auto sessionKey = makeSessionKey(suite);

	// Single packet → the one descriptor carries both start and end.
	{
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSendProvider(suite, sessionKey), {makeFrame(200, 0x10)});
		check(packets.size() == 1, "expected a single packet");
		uint8_t d = descriptorByte(packets.front());
		check(d == (S | E),
		      "single-packet descriptor should be 0xC0 (S|E), got " + std::to_string(d));
	}

	// Multiple packets → S on the first, E on the last, nothing on the middle.
	{
		auto packets =
		    packetizeFrames(RtpPacketizer::VideoClockRate, makeSendProvider(suite, sessionKey),
		                    {makeFrame(5000, 0x20)});
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
	// rejected: T=1 for packetized origin, missing S, missing E, neither. Reserved bits are not
	// in this list -- see testReservedDescriptorBitsIgnored above.
	for (uint8_t bad : {uint8_t(0xE0), uint8_t(0x40), uint8_t(0x80), uint8_t(0x00)}) {
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSendProvider(suite, sessionKey), {makeFrame(200, 0x40)});
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
		auto packets =
		    packetizeFrames(RtpPacketizer::VideoClockRate, makeSendProvider(suite, sessionKey),
		                    {makeFrame(5000, 0x50)});
		check(packets.size() > 1, "expected multiple packets");
		setDescriptorByte(packets.front(), 0x00);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(std::move(packets), provider);
		check(decoded.empty(), "multi-packet frame with a bad first descriptor should be dropped");
	}
}

// The audio path validates the descriptor too: a single-packet audio frame
// whose descriptor is not S|E (T=0) must be dropped.
void testAudioBadDescriptorIgnored() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000;
	auto sessionKey = makeSessionKey(suite);

	for (uint8_t bad : {uint8_t(0x80), uint8_t(0x40), uint8_t(0x00)}) {
		auto packets =
		    packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {makeFrame(160, 0x70)});
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
		auto config =
		    std::make_shared<RtpPacketizationConfig>(TEST_SSRC, TEST_CNAME, TEST_PT, clockRate);
		config->mid = "0"; // MID value
		config->midId = 1; // one-byte header extension id (1-14)

		SFramePerFrameRtpPacketizer packetizer(config, makeSendProvider(suite, sessionKey));

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
		check(decoded.size() == 1,
		      "expected 1 decrypted frame with extensions, got " + std::to_string(decoded.size()));
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
		auto packets = packetizeFrames(clockRate, makeSendProvider(suite, sessionKey), {frame});
		check(packets.size() == 1, "expected a single packet to inject CSRCs into");

		auto withCsrc = withCsrcs(packets.front(), 3);
		auto h = reinterpret_cast<const RtpHeader *>(withCsrc->data());
		check(h->csrcCount() == 3, "expected 3 CSRCs on the rewritten packet");

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		message_vector input{withCsrc};
		auto decoded = audio ? depacketizeAudio(std::move(input), clockRate, provider)
		                     : depacketizeVideo(std::move(input), provider);
		check(decoded.size() == 1,
		      "expected 1 decrypted frame with CSRCs, got " + std::to_string(decoded.size()));
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

	SFramePerFrameRtpPacketizer packetizer(config, makeSendProvider(suite, sessionKey));

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
		SFramePerFrameRtpPacketizer packetizer(config, makeSendProvider(suite, sessionKey));

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
		check(decoded.size() == 1,
		      "expected 1 frame with header extensions, got " + std::to_string(decoded.size()));
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

	for (uint8_t pad :
	     {uint8_t(1), uint8_t(2), uint8_t(4), uint8_t(16), uint8_t(64), uint8_t(255)}) {
		// Video, single packet.
		{
			auto frame = makeFrame(200, 0x80);
			auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
			                               makeSendProvider(suite, sessionKey), {frame});
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
			auto packets = packetizeFrames(48000, makeSendProvider(suite, sessionKey), {frame});
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
		                               makeSendProvider(suite, sessionKey), {frame});
		check(packets.size() > 1, "expected a fragmented video frame");
		for (auto &pkt : packets)
			pkt = withPadding(pkt, 8);

		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(std::move(packets), provider);
		check(decoded.size() == 1,
		      "expected 1 reassembled padded frame, got " + std::to_string(decoded.size()));
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
	                               makeSendProvider(suite, sessionKey), {frame});
	check(packets.size() == 1, "expected a single packet");

	// Set the P bit and claim a padding count larger than the whole packet.
	auto pkt = packets.front();
	(*pkt)[0] = static_cast<std::byte>(static_cast<uint8_t>((*pkt)[0]) | 0x20);
	(*pkt)[pkt->size() - 1] = static_cast<std::byte>(0xFF); // 255 > payload

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	message_vector input{pkt};
	auto decoded = depacketizeVideo(std::move(input), provider);
	check(decoded.empty(), "packet with an over-large padding count must be dropped, got " +
	                           std::to_string(decoded.size()));
}

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
	                                    makeSendProvider(suite, sessionKey), {videoFrame});
	auto audioPackets = packetizeFramesWithSsrc(audioClockRate, TEST_SSRC + 1,
	                                            makeSendProvider(suite, sessionKey), {audioFrame});

	SFramePerFrameVideoRtpDepacketizer videoDepack(provider);
	SFramePerFrameAudioRtpDepacketizer audioDepack(audioClockRate, provider);

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
	auto firstPackets =
	    packetizeFrames(RtpPacketizer::VideoClockRate, makeSendProvider(suite, firstKey), {frame});
	auto secondPackets =
	    packetizeFrames(RtpPacketizer::VideoClockRate, makeSendProvider(suite, secondKey), {frame});

	auto provider = std::make_shared<MutableKeyProvider>(suite, firstKey);
	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

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

	SFramePerFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
	                                       makeSendProvider(suite, sessionKey));

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
	check(decoded.size() == 1, "expected 1 decrypted frame, got " + std::to_string(decoded.size()));
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
		SFramePerFrameRtpPacketizer sframePacketizer(config, makeSendProvider(suite, sessionKey));
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
	check(payloadSize == frame.size(), "plain packetizer emitted " + std::to_string(payloadSize) +
	                                       " payload bytes for a " + std::to_string(frame.size()) +
	                                       " byte frame, so SFrame framing leaked onto it");
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

	SFramePerFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
	                                       makeSendProvider(suite, sessionKey), maxFragmentSize);

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
		check(payloadSize <= maxFragmentSize, "payload of " + std::to_string(payloadSize) +
		                                          " bytes exceeds the " +
		                                          std::to_string(maxFragmentSize) + " byte limit");
	}

	// Still decrypts, so the smaller chunks reassemble correctly
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideo(messages, provider);
	check(decoded.size() == 1, "expected 1 decrypted frame, got " + std::to_string(decoded.size()));
	check(sameBytes(frame, decoded.front()), "fragmented frame did not round-trip");
}

// addSFrame() must not duplicate the line across renegotiation, and the packetizer must encrypt
// whatever the description says -- there is no state in which it emits plaintext.
void testSFrameNegotiationAttribute() {
	// addSFrame() is idempotent across renegotiation
	{
		Description::Video media("0", Description::Direction::SendOnly);
		media.addVideoCodec(TEST_PT, "H264");
		media.addSFrame();
		media.addSFrame();
		media.addSFrame();

		const string sdp = string(media);
		size_t count = 0;
		for (size_t pos = sdp.find("a=sframe"); pos != string::npos;
		     pos = sdp.find("a=sframe", pos + 1))
			count++;

		check(media.hasSFrame(), "addSFrame() did not set the attribute");
		check(count == 1, "expected 1 a=sframe line, got " + std::to_string(count));
	}

	// The packetizer encrypts whatever the description says. There is no longer a state in which
	// it is installed and emits plaintext: an m-line whose answer declined a=sframe is stopped, so
	// a description without the attribute is one this packetizer should never have been given.
	// Asserting it here is what stops the old "fall back to the base packetizer" behaviour from
	// being reintroduced, which would put media in the clear under a track that claimed protection.
	{
		const uint8_t suite = 0x04;
		auto sessionKey = makeSessionKey(suite);
		auto frame = makeFrame(120, 0x66);

		SFramePerFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
		                                       makeSendProvider(suite, sessionKey));

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
		const size_t withoutAttribute = payloadOf(negotiated);
		check(withoutAttribute > frame.size(),
		      "payload is " + std::to_string(withoutAttribute) + " bytes for a " +
		          std::to_string(frame.size()) +
		          " byte frame: the packetizer stopped encrypting because the description carried "
		          "no a=sframe, which is the downgrade that must no longer exist");

		negotiated.addSFrame();
		const size_t withAttribute = payloadOf(negotiated);
		check(withAttribute == withoutAttribute,
		      "the attribute changed the packetizer's output (" + std::to_string(withoutAttribute) +
		          " vs " + std::to_string(withAttribute) + " bytes), so it is still conditional");
	}

}

void testFailClosedOnMissingKeyMaterial() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);

	// Send side: an empty base key is rejected when the provider is built, before any packetizer
	// exists to accept it.
	{
		bool threw = false;
		try {
			auto empty = std::make_shared<SFrameSendKeyProvider>(
			    suite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
			    SFrameSendKey{binary{}, /*kid=*/0});
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "the send provider accepted an empty base key");
	}

	// Send side: a null key provider is rejected at construction too, so a packetizer can never
	// exist without something to encrypt under -- there is no descriptor-only passthrough mode it
	// could fall back to while the peer believes the track is protected.
	{
		bool threw = false;
		try {
			SFramePerFrameRtpPacketizer packetizer(makeConfig(RtpPacketizer::VideoClockRate),
			                                       nullptr);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "the packetizer accepted a null key provider");
	}

	// Receive side: a null key provider is rejected at construction
	{
		bool threw = false;
		try {
			SFramePerFrameVideoRtpDepacketizer depacketizer(nullptr);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "video depacketizer accepted a null key provider");
	}

	{
		bool threw = false;
		try {
			SFramePerFrameAudioRtpDepacketizer depacketizer(48000, nullptr);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "audio depacketizer accepted a null key provider");
	}

	// The decoder set is exported and guards itself, so a direct caller cannot build one without
	// a provider. The check lives here rather than in the decrypt helper because every decoder the
	// set hands out dereferences the provider: refusing to construct it is what keeps ciphertext
	// away from a caller that swallows the exception, since there is then nothing to decrypt with.
	{
		bool threw = false;
		try {
			impl::SFrameDecoderSet decoders(nullptr);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		check(threw, "SFrameDecoderSet accepted a null key provider");
	}

	// The guards must not be satisfiable by a blanket refusal: a correctly configured
	// pair still round-trips.
	{
		auto frame = makeFrame(200, 0x22);
		auto packets = packetizeFrames(RtpPacketizer::VideoClockRate,
		                               makeSendProvider(suite, sessionKey), {frame});
		auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
		auto decoded = depacketizeVideo(packets, provider);
		check(decoded.size() == 1,
		      "expected 1 decrypted frame, got " + std::to_string(decoded.size()));
		check(sameBytes(frame, decoded.front()), "frame did not round-trip");
	}
}

// One packet per incoming() call, as Track::incoming delivers them. Needed to interleave two
// streams, which the batch form cannot express.
message_vector depacketizeAudioPerPacket(message_vector packets, uint32_t clockRate,
                                         shared_ptr<SFrameReceiveKeyProvider> provider) {
	SFramePerFrameAudioRtpDepacketizer depacketizer(clockRate, std::move(provider));
	message_vector decoded;
	for (auto &packet : packets) {
		message_vector one{std::move(packet)};
		depacketizer.incoming(one, [](message_ptr) {});
		for (auto &out : one)
			decoded.push_back(std::move(out));
	}
	return decoded;
}

// Order between streams is not meaningful, so membership is what the two-stream tests assert.
bool containsFrame(const message_vector &decoded, const binary &frame) {
	return std::any_of(decoded.begin(), decoded.end(),
	                   [&frame](const message_ptr &m) { return sameBytes(frame, m); });
}

// Every SSRC of an m-line resolves to one track, so simulcast, RTX and FEC all arrive at one
// depacketizer. Two streams under the same RTP timestamp must stay separate: their sequence
// spaces are unrelated, so one shared group would splice their packets into a single object.
void testVideoTwoSsrcsInterleaveIndependently() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey); // per-SSRC derivation on
	const SSRC ssrcA = 0x1111AAAA;
	const SSRC ssrcB = 0x2222BBBB;

	auto frameA = makeFrame(1600, 0x81); // two packets each
	auto frameB = makeFrame(1600, 0x82);

	auto packetsA =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrcA, sendProvider, {frameA});
	auto packetsB =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrcB, sendProvider, {frameB});
	check(packetsA.size() == 2 && packetsB.size() == 2,
	      "expected two packets per frame, got " + std::to_string(packetsA.size()) + " and " +
	          std::to_string(packetsB.size()));

	// Both streams start at timestamp 1000, which is the case a timestamp-only group merges.
	message_vector interleaved;
	for (size_t i = 0; i < 2; ++i) {
		interleaved.push_back(packetsA[i]);
		interleaved.push_back(packetsB[i]);
	}

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideoPerPacket(std::move(interleaved), provider);
	check(decoded.size() == 2,
	      "expected both interleaved frames, got " + std::to_string(decoded.size()));
	check(containsFrame(decoded, frameA), "stream A's frame did not survive interleaving");
	check(containsFrame(decoded, frameB), "stream B's frame did not survive interleaving");
}

// The positive control for the staleness watermark: on ONE stream, a frame older than the last
// one delivered is dropped.
//
// Nothing asserted this. Every other test that touches the watermark asserts a frame *survived* --
// testVideoPerStreamStalenessWatermark checks the watermark is per stream, and
// testUnauthenticatedObjectDoesNotSetWatermark checks a forged object cannot move it -- so making
// isStaleSFrameTimestamp() return false unconditionally left all of them green. Worse,
// testVideoStreamTableIsBounded and testStreamMapBoundedByMalformedPackets use "the victim's
// watermark no longer rejects an older timestamp" as their only observable, so that one mutation
// silently disarmed three tests at once.
void testStaleTimestampOnSameStreamIsDropped() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	const SSRC ssrc = 0x5555EEEE;

	auto newer = makeFrame(200, 0x91);
	auto older = makeFrame(200, 0x92);

	auto packets =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrc, sendProvider, {newer, older});
	check(packets.size() == 2, "expected one packet per frame");

	// Same SSRC, and the second frame's RTP timestamp is far behind the first. Only the header is
	// rewritten, so both still authenticate -- the frame is dropped for being late, not forged.
	message_vector ordered;
	ordered.push_back(withRtpFraming(packets[0], 900000, 100, /*marker=*/true));
	ordered.push_back(withRtpFraming(packets[1], 1000, 101, /*marker=*/true));

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideoPerPacket(std::move(ordered), provider);

	check(containsFrame(decoded, newer), "the first frame did not round-trip");
	check(!containsFrame(decoded, older),
	      "a frame whose timestamp is 899000 ticks behind the last one delivered on the same "
	      "stream was accepted: the staleness watermark is not rejecting anything, which also "
	      "makes the two stream-table bound tests vacuous");
	check(decoded.size() == 1,
	      "expected exactly the newer frame, got " + std::to_string(decoded.size()));
}

// A single incoming() batch carrying two SSRCs, with per-SSRC derivation on.
//
// incoming() is public and takes a vector, so a caller batching by time rather than by SSRC is
// entitled to do this. Decrypting the whole batch under whichever SSRC happened to arrive last
// checks the other stream's frames against the wrong derived key, and they are dropped as
// authentication failures -- indistinguishable from corruption. Invisible with derivation off,
// where the SSRC selects nothing, which is what makes it easy to miss.
void testBatchSpanningTwoSsrcsDecryptsEachUnderItsOwnKey() {
	const uint8_t suite = 0x04;
	const uint8_t ratchetStepBits = 4;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/true, ratchetStepBits);
	const SSRC ssrcA = 0x7A00000A;
	const SSRC ssrcB = 0x7B00000B;

	auto frameA = makeFrame(200, 0x71);
	auto frameB = makeFrame(200, 0x72);

	auto packetsA =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrcA, sendProvider, {frameA});
	auto packetsB =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrcB, sendProvider, {frameB});
	check(packetsA.size() == 1 && packetsB.size() == 1, "expected one packet per frame");

	// Both in one call, A first so that B is the SSRC left in the local when the batch is
	// decrypted. A is the one that gets checked against the wrong key if the split is missing.
	message_vector batch;
	batch.push_back(withRtpFraming(packetsA.front(), 3000, 100, /*marker=*/true));
	batch.push_back(withRtpFraming(packetsB.front(), 6000, 200, /*marker=*/true));

	auto provider =
	    std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/true, ratchetStepBits);
	auto decoded = depacketizeVideo(std::move(batch), provider);

	check(containsFrame(decoded, frameB),
	      "the last SSRC in the batch did not round-trip, so this test is not set up correctly");
	check(containsFrame(decoded, frameA),
	      "the first SSRC's frame was lost: the batch was decrypted under the last SSRC's derived "
	      "key, so every other stream in it failed authentication");
}

// The staleness watermark is per stream: RTP timestamp spaces of two SSRCs are unrelated, so a
// high timestamp on one must not make an ordinary timestamp on the other look like a straggler.
void testVideoPerStreamStalenessWatermark() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	const SSRC ssrcA = 0x3333CCCC;
	const SSRC ssrcB = 0x4444DDDD;

	auto frameA = makeFrame(200, 0x83); // one packet each
	auto frameB = makeFrame(200, 0x84);

	auto packetsA =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrcA, sendProvider, {frameA});
	auto packetsB =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrcB, sendProvider, {frameB});
	check(packetsA.size() == 1 && packetsB.size() == 1, "expected one packet per frame");

	// A is far ahead of B. Rewriting the RTP timestamp is safe: it is header metadata and no
	// part of the SFrame ciphertext.
	message_vector ordered;
	ordered.push_back(withRtpFraming(packetsA.front(), 900000, 100, /*marker=*/true));
	ordered.push_back(withRtpFraming(packetsB.front(), 1000, 500, /*marker=*/true));

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeVideoPerPacket(std::move(ordered), provider);
	check(decoded.size() == 2, "a low timestamp on another stream was dropped as stale, got " +
	                               std::to_string(decoded.size()) + " frame(s)");
	check(containsFrame(decoded, frameA), "stream A's frame did not round-trip");
	check(containsFrame(decoded, frameB), "stream B's frame was treated as a late straggler");
}

// The audio path keeps one frame in flight, so two interleaved streams would each look out of
// sequence to the other and discard the partial on every packet.
void testAudioTwoSsrcsInterleaveIndependently() {
	const uint8_t suite = 0x01;
	const uint32_t clockRate = 48000; // Opus
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	const SSRC ssrcA = 0x5555EEEE;
	const SSRC ssrcB = 0x6666FFFF;

	auto frameA = makeFrame(2000, 0x85); // fragments into several packets
	auto frameB = makeFrame(2000, 0x86);

	auto packetsA = packetizeFramesWithSsrc(clockRate, ssrcA, sendProvider, {frameA});
	auto packetsB = packetizeFramesWithSsrc(clockRate, ssrcB, sendProvider, {frameB});
	check(packetsA.size() > 1 && packetsB.size() == packetsA.size(),
	      "expected both audio frames to fragment equally, got " + std::to_string(packetsA.size()) +
	          " and " + std::to_string(packetsB.size()));

	message_vector interleaved;
	for (size_t i = 0; i < packetsA.size(); ++i) {
		interleaved.push_back(packetsA[i]);
		interleaved.push_back(packetsB[i]);
	}

	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto decoded = depacketizeAudioPerPacket(std::move(interleaved), clockRate, provider);
	check(decoded.size() == 2,
	      "expected both interleaved audio frames, got " + std::to_string(decoded.size()));
	check(containsFrame(decoded, frameA), "audio stream A's frame did not survive interleaving");
	check(containsFrame(decoded, frameB), "audio stream B's frame did not survive interleaving");
}

// A peer is free to put a fresh SSRC on every packet, so the per-stream table is bounded. Each
// of these streams leaves a frame half open, which is what would otherwise accumulate for ever.
void testVideoStreamTableIsBounded() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	const SSRC victim = 0x7000'0000;

	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	// Seed the victim with a frame that actually authenticates, at a high timestamp: the staleness
	// watermark only advances on a frame that decoded, so an orphaned packet would leave no
	// watermark and nothing below would be observable.
	auto seed = packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, victim, sendProvider,
	                                    {makeFrame(200, 0x86)});
	check(seed.size() == 1, "expected a single seed packet");
	{
		message_vector one{withRtpFraming(seed.front(), 900000, 10, /*marker=*/true)};
		depacketizer.incoming(one, [](message_ptr) {});
		check(one.size() == 1, "the seed frame did not decode, so no watermark was established");
	}

	// Churn past the cap with orphaned first packets, which do get buffered -- a different way into
	// streamFor() than the malformed packets the sibling test uses, which are dropped before
	// buffering.
	for (SSRC ssrc = 0x7000'0001; ssrc < 0x7000'0021; ++ssrc) {
		auto orphan = makeFrame(1600, 0x87); // two packets, only the first is delivered
		auto packets =
		    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrc, sendProvider, {orphan});
		message_vector one{packets.front()};
		depacketizer.incoming(one, [](message_ptr) {});
	}

	// A complete frame on a fresh stream still arrives after all that churn.
	auto frame = makeFrame(200, 0x88);
	auto packets =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, 0x7000'00FF, sendProvider, {frame});
	message_vector last{packets.front()};
	depacketizer.incoming(last, [](message_ptr) {});
	check(last.size() == 1, "a complete frame was lost after many streams, got " +
	                            std::to_string(last.size()) + " frame(s)");
	check(sameBytes(frame, last.front()), "frame after stream churn did not round-trip");

	// That assertion holds whether or not the table is bounded, so it does not test the cap. The
	// victim's watermark is what makes eviction observable: still seated at 900000 it would reject
	// the older timestamp below, so the frame arriving proves the entry was evicted. Deleting the
	// eviction loop in streamFor() leaves the watermark in place and fails here.
	auto later = packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, victim, sendProvider,
	                                     {makeFrame(200, 0x89)});
	message_vector stale{withRtpFraming(later.front(), 1000, 11, /*marker=*/true)};
	depacketizer.incoming(stale, [](message_ptr) {});
	check(stale.size() == 1,
	      "the victim stream's watermark outlived the churn, so the stream table is not bounded "
	      "where its entries are created");
}

// A flood of packets that can never resolve must not wedge the stream: a real frame afterwards has
// to decode. That is what this pins, and it is worth pinning -- but note it does not test the
// packet cap, because an unresolvable group is released by the end-of-call flush whether the cap
// exists or not. testFrameSpanningPastTheGroupCapIsRefused() below is the test that the cap itself
// acts.
void testUnresolvableGroupIsCapped() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	const SSRC ssrc = 0x9000'0001;

	// One real packet to clone framing from.
	auto seed = packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrc, sendProvider,
	                                    {makeFrame(200, 0x8A)});
	check(seed.size() == 1, "expected a single seed packet");

	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	// All under one timestamp, well before the real frame's, and every packet an E with no S: a
	// continuation that ends a run which never started. The walk charges each one as loss and
	// clears the run, so the group can never resolve and never leaves the front of the queue.
	const size_t sent = 5000; // above MaxSFramePacketsPerGroup, so the cap fires
	for (size_t i = 0; i < sent; ++i) {
		auto pkt = withRtpFraming(seed.front(), 500, uint16_t(i), /*marker=*/false);
		setDescriptorByte(pkt, impl::SFrameDescriptorE);
		message_vector one{std::move(pkt)};
		depacketizer.incoming(one, [](message_ptr) {});
	}

	// A complete frame on the same stream still arrives, so the cap dropped the junk rather than
	// wedging the stream.
	auto frame = makeFrame(200, 0x8B);
	auto packets =
	    packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrc, sendProvider, {frame});
	message_vector last{packets.front()};
	depacketizer.incoming(last, [](message_ptr) {});
	check(last.size() == 1, "a complete frame was lost after an unresolvable group, got " +
	                            std::to_string(last.size()) + " frame(s)");
	check(sameBytes(frame, last.front()), "frame after the capped group did not round-trip");
}

// The cap's contract is that a real frame cannot span that many packets, so one that does is
// refused rather than assembled. This is the differential the flood above cannot provide: a group
// that never resolves is dropped either way, so only a group that *would* have resolved shows the
// cap acting. Fragmenting at 3 bytes -- one descriptor plus two payload bytes -- is the cheapest
// way to push a legitimate frame past the cap.
void testFrameSpanningPastTheGroupCapIsRefused() {
	// Must track MaxSFramePacketsPerGroup in sframeperframevideortpdepacketizer.cpp, which is
	// file-local there and so cannot be referenced from here.
	const size_t groupCap = 2048;

	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	const SSRC ssrc = 0x9100'0001;

	auto config = std::make_shared<RtpPacketizationConfig>(ssrc, TEST_CNAME, TEST_PT,
	                                                       RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizer(config, sendProvider, /*maxFragmentSize=*/3);

	auto frame = makeFrame(4400, 0x8C);
	auto frameInfo = std::make_shared<FrameInfo>(1000);
	frameInfo->payloadType = TEST_PT;
	message_vector messages{make_message(binary(frame), frameInfo)};
	packetizer.outgoing(messages, [](message_ptr) {});
	check(messages.size() > groupCap, "expected the frame to span past the cap, got " +
	                                      std::to_string(messages.size()) + " packets");

	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);
	size_t delivered = 0;
	for (auto &pkt : messages) {
		message_vector one{pkt};
		depacketizer.incoming(one, [](message_ptr) {});
		delivered += one.size();
	}

	check(
	    delivered == 0,
	    "a frame spanning more than the group cap was assembled, so the cap did not drop it; got " +
	        std::to_string(delivered) + " frame(s)");
}

// A stream that goes quiet while its siblings keep sending must still decode when it comes back.
//
// Without the Section 7 derivation the SSRC is no part of the key schedule: one master key, one
// shared encoder, one ratchet schedule for the whole m-line. The sender therefore ratchets while
// this stream is idle -- driven by the others' frames -- and resumes it at the current step. A
// receiver that kept a chain per SSRC would have this one still sitting at the step it last saw,
// need an advance past MaxForwardRatchet to rejoin, and be refused it, because the busy streams
// keep the shared catch-up allowance looking live. One decoder for the m-line is what makes the
// resume cost nothing.
void testIdleStreamResumesWithoutPerSsrcDerivation() {
	const uint8_t suite = 0x04;
	const uint8_t ratchetStepBits = 8;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/false, ratchetStepBits,
	                                     /*ratchetPeriod=*/1);
	const SSRC busy = 0xC1000001;
	const SSRC idle = 0xC2000002;

	auto configBusy = std::make_shared<RtpPacketizationConfig>(busy, TEST_CNAME, TEST_PT,
	                                                           RtpPacketizer::VideoClockRate);
	auto configIdle = std::make_shared<RtpPacketizationConfig>(idle, TEST_CNAME, TEST_PT,
	                                                           RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizerBusy(configBusy, sendProvider);
	SFramePerFrameRtpPacketizer packetizerIdle(configIdle, sendProvider);

	auto provider =
	    std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/false, ratchetStepBits);
	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	uint32_t timestamp = 3000;
	uint64_t lastKid = 0;
	auto sendOne = [&](SFramePerFrameRtpPacketizer &packetizer, uint8_t seed) {
		auto frame = makeFrame(200, seed);
		auto frameInfo = std::make_shared<FrameInfo>(timestamp);
		frameInfo->payloadType = TEST_PT;
		timestamp += 3000;
		message_vector messages{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(messages, [](message_ptr) {});
		check(messages.size() == 1, "expected a single packet per frame");
		// The KID names the ratchet step this frame went out under, which is what makes the
		// shared schedule observable: decoding alone does not distinguish a stream that caught
		// up to its siblings from one that advanced a single step of its own.
		lastKid = kidOf(messages.front());
		message_vector one{messages.front()};
		depacketizer.incoming(one, [](message_ptr) {});
		size_t got = 0;
		for (auto &out : one)
			if (out && out->type != Message::Control && out->frameInfo) {
				check(sameBytes(frame, out), "a frame did not round-trip");
				got++;
			}
		return got;
	};

	// The idle stream decodes once first. Without this its chain would not exist when it returns,
	// and a chain that has never authenticated is allowed the long catch-up walk -- so the test
	// would pass whether or not the divergence it is named for was fixed.
	check(sendOne(packetizerIdle, 0xD0) == 1, "the idle stream's first frame did not decode");

	// Four ratchet periods driven entirely by the other stream, so the shared encoder ends well
	// past MaxForwardRatchet, and the busy stream keeps refreshing the shared allowance.
	for (int round = 0; round < 4; ++round) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		check(sendOne(packetizerBusy, uint8_t(0xE0 + round)) == 1,
		      "the busy stream stopped decoding while ratcheting");
	}

	const uint64_t busyKid = lastKid;

	check(sendOne(packetizerIdle, 0xDF) == 1,
	      "the stream that went quiet could not rejoin after the others ratcheted past it");

	// Same reasoning as the per-SSRC test: without this an encoder advancing one step per frame
	// decodes fine and the assertion above passes while the shared schedule is broken. With
	// derivation off both streams share one encoder, so the KIDs must match for that reason too.
	check(lastKid == busyKid,
	      "the stream returned on KID " + std::to_string(lastKid) + " but its siblings are on " +
	          std::to_string(busyKid) + ": the ratchet step is not shared across the m-line");
}

// The mirror with the derivation on, where the keys are per SSRC but the ratchet step is not: any
// stream sending advances it for the whole m-line, so all of them carry one KID
// (draft-ietf-avtcore-rtp-sframe Section 8). A stream that was idle is therefore behind by whatever
// the others accumulated and has to walk its own key forward to rejoin, which is allowed because
// those steps have already authenticated on this m-line -- the chains stay separate, the step does
// not.
void testIdleStreamResumesWithPerSsrcDerivation() {
	const uint8_t suite = 0x04;
	const uint8_t ratchetStepBits = 8;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/true, ratchetStepBits,
	                                     /*ratchetPeriod=*/1);
	const SSRC busy = 0xC3000003;
	const SSRC idle = 0xC4000004;

	auto configBusy = std::make_shared<RtpPacketizationConfig>(busy, TEST_CNAME, TEST_PT,
	                                                           RtpPacketizer::VideoClockRate);
	auto configIdle = std::make_shared<RtpPacketizationConfig>(idle, TEST_CNAME, TEST_PT,
	                                                           RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizerBusy(configBusy, sendProvider);
	SFramePerFrameRtpPacketizer packetizerIdle(configIdle, sendProvider);

	auto provider =
	    std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/true, ratchetStepBits);
	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	uint32_t timestamp = 3000;
	uint64_t lastKid = 0;
	auto sendOne = [&](SFramePerFrameRtpPacketizer &packetizer, uint8_t seed) {
		auto frame = makeFrame(200, seed);
		auto frameInfo = std::make_shared<FrameInfo>(timestamp);
		frameInfo->payloadType = TEST_PT;
		timestamp += 3000;
		message_vector messages{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(messages, [](message_ptr) {});
		check(messages.size() == 1, "expected a single packet per frame");
		// The KID names the ratchet step this frame went out under, which is what makes the
		// shared schedule observable: decoding alone does not distinguish a stream that caught
		// up to its siblings from one that advanced a single step of its own.
		lastKid = kidOf(messages.front());
		message_vector one{messages.front()};
		depacketizer.incoming(one, [](message_ptr) {});
		size_t got = 0;
		for (auto &out : one)
			if (out && out->type != Message::Control && out->frameInfo) {
				check(sameBytes(frame, out), "a frame did not round-trip");
				got++;
			}
		return got;
	};

	check(sendOne(packetizerIdle, 0xF0) == 1, "the idle stream's first frame did not decode");

	for (int round = 0; round < 4; ++round) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		check(sendOne(packetizerBusy, uint8_t(0xA0 + round)) == 1,
		      "the busy stream stopped decoding while ratcheting");
	}

	const uint64_t busyKid = lastKid;

	// The siblings advanced the shared step while this one was quiet, so it returns several steps
	// ahead of its own chain and must be allowed to walk there.
	check(sendOne(packetizerIdle, 0xFF) == 1,
	      "the quiet stream could not rejoin with per-SSRC derivation: the shared step moved while "
	      "it was idle and its chain was refused the walk to catch up");

	// Decoding is not enough on its own. An encoder that advanced one step per frame instead of
	// jumping to the m-line's target -- turning the catch-up `while` into an `if`, say -- would
	// return at step 1, be within MaxForwardRatchet of its chain head, and decode perfectly well
	// while carrying a KID none of its siblings ever used. The shared step is the thing under
	// test, so the KID is what has to match.
	check(lastKid == busyKid,
	      "the stream returned on KID " + std::to_string(lastKid) + " but its siblings are on " +
	          std::to_string(busyKid) +
	          ": it advanced on its own schedule instead of catching up to the m-line's");
}

// The sibling above rejoined at a step another stream had already proven. This one rejoins by
// opening a ratchet period itself, so it arrives one step *past* the proven mark -- the returning
// stream is the one that trips the timer, and its own frame is what advances the shared step. A
// reference of "at or below proven" refuses exactly this frame, and because lastDecodeOk is
// per-m-line and the busy sibling keeps it fresh, the stale path does not rescue it either: the
// stream stays dark until the siblings stop for CatchUpAfter.
void testStreamOpeningARatchetPeriodOnReturnDecodes() {
	const uint8_t suite = 0x04;
	const uint8_t ratchetStepBits = 8;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/true, ratchetStepBits,
	                                     /*ratchetPeriod=*/1);
	const SSRC busy = 0xC5000005;
	const SSRC idle = 0xC6000006;

	auto configBusy = std::make_shared<RtpPacketizationConfig>(busy, TEST_CNAME, TEST_PT,
	                                                           RtpPacketizer::VideoClockRate);
	auto configIdle = std::make_shared<RtpPacketizationConfig>(idle, TEST_CNAME, TEST_PT,
	                                                           RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizerBusy(configBusy, sendProvider);
	SFramePerFrameRtpPacketizer packetizerIdle(configIdle, sendProvider);

	auto provider =
	    std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/true, ratchetStepBits);
	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	uint32_t timestamp = 3000;
	uint64_t lastKid = 0;
	auto sendOne = [&](SFramePerFrameRtpPacketizer &packetizer, uint8_t seed) {
		auto frame = makeFrame(200, seed);
		auto frameInfo = std::make_shared<FrameInfo>(timestamp);
		frameInfo->payloadType = TEST_PT;
		timestamp += 3000;
		message_vector messages{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(messages, [](message_ptr) {});
		check(messages.size() == 1, "expected a single packet per frame");
		// The KID names the ratchet step this frame went out under, which is what makes the
		// shared schedule observable: decoding alone does not distinguish a stream that caught
		// up to its siblings from one that advanced a single step of its own.
		lastKid = kidOf(messages.front());
		message_vector one{messages.front()};
		depacketizer.incoming(one, [](message_ptr) {});
		size_t got = 0;
		for (auto &out : one)
			if (out && out->type != Message::Control && out->frameInfo) {
				check(sameBytes(frame, out), "a frame did not round-trip");
				got++;
			}
		return got;
	};

	check(sendOne(packetizerIdle, 0xD0) == 1, "the idle stream's first frame did not decode");

	for (int round = 0; round < 4; ++round) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		check(sendOne(packetizerBusy, uint8_t(0xB0 + round)) == 1,
		      "the busy stream stopped decoding while ratcheting");
	}

	// Let the period elapse without the busy stream sending, so the returning stream's own frame
	// is what advances the shared step. It therefore arrives one past what any sibling has proven,
	// while lastDecodeOk is only a second old.
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	check(sendOne(packetizerIdle, 0xDF) == 1,
	      "the returning stream that opened the ratchet period was refused: it sits one step past "
	      "the proven mark, and the busy sibling kept lastDecodeOk too fresh for the stale path");
}

// A stream whose decoder was evicted must still decode when it returns, even past a wrap of the
// KID's ratchet step field.
//
// The wire carries only the low R bits of the step, so unwrapping needs a reference near the
// sender's real position. A rebuilt chain has a head of zero, which makes both wrap branches of
// unwrapRatchetStep() unreachable and resolves every step to `wireStep mod 2^R` -- correct until
// the sender wraps, then wrong by a multiple of the field size, and permanently so, because the
// chain only commits on a frame that authenticates. The furthest step proven on the m-line is the
// reference that replaces the lost head.
//
// R is 3 here, the narrowest the unwrap supports, so the field wraps every eighth ratchet and a few
// seconds of traffic crosses it. MaxDecoders is well above the stream count, so eviction is forced
// by driving more SSRCs than the set holds.
void testEvictedStreamResumesPastAFieldWrap() {
	const uint8_t suite = 0x04;
	const uint8_t ratchetStepBits = 3; // field of 8 steps
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/true, ratchetStepBits,
	                                     /*ratchetPeriod=*/1);
	const SSRC victim = 0xE1000001;

	auto provider =
	    std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/true, ratchetStepBits);
	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	uint32_t timestamp = 3000;
	uint64_t lastKid = 0;
	auto sendFrom = [&](SSRC ssrc, uint8_t seed) {
		auto config = std::make_shared<RtpPacketizationConfig>(ssrc, TEST_CNAME, TEST_PT,
		                                                       RtpPacketizer::VideoClockRate);
		SFramePerFrameRtpPacketizer packetizer(config, sendProvider);
		auto frame = makeFrame(200, seed);
		auto frameInfo = std::make_shared<FrameInfo>(timestamp);
		frameInfo->payloadType = TEST_PT;
		timestamp += 3000;
		message_vector messages{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(messages, [](message_ptr) {});
		check(messages.size() == 1, "expected a single packet per frame");
		lastKid = kidOf(messages.front());
		message_vector one{messages.front()};
		depacketizer.incoming(one, [](message_ptr) {});
		size_t got = 0;
		for (auto &out : one)
			if (out && out->type != Message::Control && out->frameInfo) {
				check(sameBytes(frame, out), "a frame did not round-trip");
				got++;
			}
		return got;
	};

	// The victim decodes once, so its chain exists and is established.
	check(sendFrom(victim, 0x10) == 1, "the victim's first frame did not decode");

	// Cross the field wrap: nine ratchet periods on a filler stream takes the shared step past 8.
	const SSRC filler = 0xE1000002;
	const uint64_t ratchetPeriods = 9;
	for (uint64_t i = 0; i < ratchetPeriods; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		check(sendFrom(filler, uint8_t(0x20 + i)) == 1, "the filler stream stopped decoding");
	}

	// The wrap is the point of this test and was previously only assumed. Below 2^R the unwrap
	// reference is unnecessary -- removing it resolves the same step either way -- so if the shared
	// step never crosses the field, half of what this test claims to cover silently lapses.
	const uint64_t fieldSize = uint64_t(1) << ratchetStepBits;
	check(sendProvider->currentKey() == nullopt,
	      "per-SSRC derivation should leave currentKey() empty, so the step has to come off the wire");
	check(impl::sframe::RatchetStepFromKid(lastKid, ratchetStepBits) < fieldSize,
	      "the wire step is not masked into the ratchet field, so this test is not measuring a wrap");
	check(ratchetPeriods > fieldSize,
	      "only " + std::to_string(ratchetPeriods) + " ratchet periods for a field of " +
	          std::to_string(fieldSize) +
	          ": the sender never crosses the wrap, so the unwrap reference is not under test");

	// Force the victim's decoder out of the set: one fresh SSRC per slot, each idler than the
	// filler, so the victim -- untouched since before the wrap -- is the one evicted. Each is
	// itself a stream arriving past the wrap against a fresh chain, so it exercises the
	// proven-step exemption; discarding the result would hide all forty of them failing.
	const uint32_t fillerSsrcs = 40;
	for (uint32_t i = 0; i < fillerSsrcs; ++i)
		check(sendFrom(0xE2000000 + i, uint8_t(0x40 + (i % 16))) == 1,
		      "a fresh SSRC arriving past the wrap did not decode, so the eviction loop is not "
		      "doing what it claims");
	check(fillerSsrcs > impl::sframe::MaxSFrameStreams,
	      "only " + std::to_string(fillerSsrcs) + " SSRCs for a decoder table of " +
	          std::to_string(impl::sframe::MaxSFrameStreams) +
	          ": the victim is no longer guaranteed to be evicted, so this has quietly become a "
	          "duplicate of the idle-stream test (MaxDecoders is defined as MaxSFrameStreams)");

	// Its chain is gone and the sender is past the wrap, so a head of zero would resolve the step
	// to `wireStep mod 8` and never authenticate again.
	check(
	    sendFrom(victim, 0x11) == 1,
	    "the victim could not resume after its decoder was evicted past a wrap of the step field");
}

// A decoder's ratchet chain is derived from its SSRC, so one decoder shared across streams clears
// the chain whenever two SSRCs alternate. With ratcheting on, a cleared chain restarts at step 0
// and only the rate-limited catch-up walk can recover it. The two-SSRC tests above cannot see this:
// they leave ratcheting off, where a cleared chain costs a re-derivation and nothing more.
//
// Each stream keeps ONE packetizer for the whole test. A fresh packetizer per frame would build a
// fresh encoder at ratchet step 0, so the sender would never advance and the alternation would cost
// nothing.
void testTwoSsrcsWithRatchetingBothDecode() {
	const uint8_t suite = 0x04;
	const uint8_t ratchetStepBits = 8;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey, /*perSsrc=*/true, ratchetStepBits,
	                                     /*ratchetPeriod=*/1);
	const SSRC ssrcA = 0xA1000001;
	const SSRC ssrcB = 0xB2000002;

	auto configA = std::make_shared<RtpPacketizationConfig>(ssrcA, TEST_CNAME, TEST_PT,
	                                                        RtpPacketizer::VideoClockRate);
	auto configB = std::make_shared<RtpPacketizationConfig>(ssrcB, TEST_CNAME, TEST_PT,
	                                                        RtpPacketizer::VideoClockRate);
	SFramePerFrameRtpPacketizer packetizerA(configA, sendProvider);
	SFramePerFrameRtpPacketizer packetizerB(configB, sendProvider);

	auto provider =
	    std::make_shared<SessionKeyProvider>(suite, sessionKey, /*perSSRC=*/true, ratchetStepBits);
	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	auto sendOne = [&](SFramePerFrameRtpPacketizer &packetizer, uint32_t timestamp,
	                   uint8_t seed) -> std::pair<binary, message_vector> {
		auto frame = makeFrame(200, seed);
		auto frameInfo = std::make_shared<FrameInfo>(timestamp);
		frameInfo->payloadType = TEST_PT;
		message_vector messages{make_message(binary(frame), frameInfo)};
		packetizer.outgoing(messages, [](message_ptr) {});
		return {frame, std::move(messages)};
	};

	// Four ratchet periods, so the sender ends well past MaxForwardRatchet and a cleared chain
	// cannot be recovered by the small forward allowance alone.
	size_t decoded = 0;
	uint32_t timestamp = 3000;
	for (int round = 0; round < 4; ++round) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		for (auto *packetizer : {&packetizerA, &packetizerB}) {
			auto [frame, packets] = sendOne(*packetizer, timestamp, uint8_t(0xC0 + round));
			timestamp += 3000;
			check(packets.size() == 1, "expected a single packet per frame");
			message_vector one{packets.front()};
			depacketizer.incoming(one, [](message_ptr) {});
			for (auto &out : one)
				if (out && out->type != Message::Control && out->frameInfo) {
					check(sameBytes(frame, out), "a ratcheted frame did not round-trip");
					decoded++;
				}
		}
	}

	check(decoded == 8, "expected all 8 ratcheted frames across two SSRCs, got " +
	                        std::to_string(decoded) +
	                        " -- a shared decoder clears the other stream's chain on every "
	                        "alternation");
}

// A packet dropped before it is buffered -- malformed, or a stale timestamp -- returns from
// incoming() without reaching the byte/group evictor, so whatever created its stream entry has to
// bound the map itself. Otherwise a peer puts a fresh SSRC on every such packet and the map grows
// without limit. The observable consequence of the bound is that the oldest streams are forgotten:
// a stream's staleness watermark does not survive thousands of other SSRCs arriving after it.
void testStreamMapBoundedByMalformedPackets() {
	const uint8_t suite = 0x04;
	auto sessionKey = makeSessionKey(suite);
	auto sendProvider = makeSendProvider(suite, sessionKey);
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	const SSRC victim = 0xD000'0001;

	SFramePerFrameVideoRtpDepacketizer depacketizer(provider);

	// Establish a watermark on the victim stream with a real frame at a high timestamp.
	auto seed = packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, victim, sendProvider,
	                                    {makeFrame(200, 0x91)});
	check(seed.size() == 1, "expected a single seed packet");
	{
		message_vector one{withRtpFraming(seed.front(), 900000, 10, /*marker=*/true)};
		depacketizer.incoming(one, [](message_ptr) {});
		check(one.size() == 1, "the seed frame did not decode");
	}

	// A 13-byte packet is a bare RTP header plus one byte: too short to parse as SFrame and not
	// payload-less either, so it is dropped before buffering. One per SSRC, thousands of SSRCs.
	auto header = reinterpret_cast<const RtpHeader *>(seed.front()->data());
	const size_t headerSize = header->getSize();
	for (uint32_t i = 0; i < 4000; ++i) {
		binary junk(seed.front()->begin(), seed.front()->begin() + headerSize + 1);
		auto pkt = make_message(std::move(junk), Message::Binary);
		auto hdr = reinterpret_cast<RtpHeader *>(pkt->data());
		hdr->setSsrc(0xE0000000 + i);
		hdr->setTimestamp(500);
		hdr->setSeqNumber(uint16_t(i));
		message_vector one{std::move(pkt)};
		depacketizer.incoming(one, [](message_ptr) {});
	}

	// The victim's entry is long gone, so its watermark no longer rejects an older timestamp --
	// which is how the bound shows from outside. Unbounded, the entry would still be there and the
	// frame below would be dropped as stale.
	auto later = packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, victim, sendProvider,
	                                     {makeFrame(200, 0x92)});
	message_vector one{withRtpFraming(later.front(), 1000, 11, /*marker=*/true)};
	depacketizer.incoming(one, [](message_ptr) {});
	check(one.size() == 1,
	      "the victim stream's watermark outlived 4000 other SSRCs, so the stream map is not "
	      "bounded where the entries are created");
}

// SFrame end to end through the real RTX machinery: RtcpNackResponder stores the outgoing
// packets and answers a NACK with an RFC 4588 retransmission on the RTX SSRC, and
// RtcpReceivingSession restores the original SSRC, sequence number and payload type before the
// SFrame depacketizer sees it. Without that unwrapping the 2-byte OSN would be read as the
// SFrame descriptor and the key derived from the RTX SSRC.
void testSFrameWithRtxRetransmission() {
	const uint8_t suite = 0x04;
	const uint8_t rtxPt = TEST_PT + 1;
	const SSRC ssrc = 0x0A0A0A0A;
	const SSRC rtxSsrc = 0x0B0B0B0B;
	auto sessionKey = makeSessionKey(suite);

	// An m-line carrying both SFrame and RTX, which is what configures both handlers.
	Description::Video media(TEST_CNAME, Description::Direction::SendRecv);
	media.addH264Codec(TEST_PT);
	media.addRtxCodec(rtxPt, TEST_PT, RtpPacketizer::VideoClockRate);
	media.addSSRC(ssrc, TEST_CNAME);
	media.addRtxSSRC(ssrc, rtxSsrc, TEST_CNAME);
	media.addSFrame();
	check(media.isRtxEnabled(), "RTX should be enabled on the test m-line");

	auto frame = makeFrame(1600, 0x89); // two packets, so one can be lost
	auto packets = packetizeFramesWithSsrc(RtpPacketizer::VideoClockRate, ssrc,
	                                       makeSendProvider(suite, sessionKey), {frame});
	check(packets.size() == 2, "expected two packets, got " + std::to_string(packets.size()));

	const uint16_t lostSeq = reinterpret_cast<const RtpHeader *>(packets[1]->data())->seqNumber();

	// Send side: the responder stores what goes out, then answers a NACK for the lost one.
	RtcpNackResponder responder;
	responder.media(media);
	{
		message_vector outgoing = packets;
		responder.outgoing(outgoing, [](message_ptr) {});
	}

	binary nackData(RtcpNack::Size(1));
	auto nack = reinterpret_cast<RtcpNack *>(nackData.data());
	nack->preparePacket(ssrc, 1);
	unsigned int fciCount = 0;
	uint16_t fciPID = lostSeq;
	nack->addMissingPacket(&fciCount, &fciPID, lostSeq);

	message_vector retransmitted;
	message_vector nackMessages{make_message(std::move(nackData), Message::Control)};
	responder.incoming(nackMessages, [&retransmitted](message_ptr m) {
		if (m && m->type != Message::Control)
			retransmitted.push_back(std::move(m));
	});
	check(retransmitted.size() == 1, "the NACK should have produced one retransmission, got " +
	                                     std::to_string(retransmitted.size()));

	auto rtxHeader = reinterpret_cast<const RtpHeader *>(retransmitted.front()->data());
	check(rtxHeader->ssrc() == rtxSsrc, "the retransmission should carry the RTX SSRC");
	check(rtxHeader->payloadType() == rtxPt,
	      "the retransmission should carry the RTX payload type");

	// Receive side: the RTX session must unwrap before the SFrame depacketizer, so it is
	// chained after -- incoming() runs from the tail of the chain toward the head.
	auto provider = std::make_shared<SessionKeyProvider>(suite, sessionKey);
	auto depacketizer = std::make_shared<SFramePerFrameVideoRtpDepacketizer>(provider);
	depacketizer->addToChain(std::make_shared<RtcpReceivingSession>());
	depacketizer->mediaChain(media);

	// The first packet arrives normally; the second only as the retransmission.
	message_vector decoded;
	for (auto &packet : {packets[0], retransmitted.front()}) {
		message_vector one{packet};
		depacketizer->incomingChain(one, [](message_ptr) {});
		for (auto &out : one)
			if (out && out->type != Message::Control)
				decoded.push_back(std::move(out));
	}

	check(decoded.size() == 1, "expected the frame to be recovered from the retransmission, got " +
	                               std::to_string(decoded.size()) + " frame(s)");
	check(sameBytes(frame, decoded.front()),
	      "the frame recovered through RTX did not decrypt to the original");
}

} // namespace

TestResult test_sframe_packetizer() {
	InitLogger(LogLevel::Warning);
	try {
		testSmallFrameRoundTrip();
		testFarFutureTimestampDoesNotWedgeVideo();
		testPerSsrcDerivationOffRoundTrips();
		testLargeFrameRoundTrip();
		testMultipleFramesRoundTrip();
		testCipherSuitesRoundTrip();
		testTamperedFrameDropped();
		testAudioFrameRoundTrip();
		testClockRateHonouredInRtpTimestamps();
		testAudioDepacketizerUsesConstructedClockRate();
		testAudioMultiPacketReassembly();
		testAudioOutOfSequenceFragmentDropped();
		testRtcpPassesThroughDepacketizers();
		testSharedKeyCountersAreUnique();
		testSharedKeyRoundTrip();
		testSharedKeyRatchets();
		testSharedEncoderRekeyMovesAllTracks();
		testWireBytesAreCiphertext();
		testMaxFragmentSizeBoundary();
		testMaxFragmentSizeClampedToOneBytePayload();
		testHeaderGrowthMidStreamStaysUnderMtu();
		testMaxHeaderStaysUnderMtu();
		testKeyScopeMismatchFails();
		testVideoReorderAcrossFrameBoundary();
		testUnauthenticatedObjectDoesNotSetWatermark();
		testVideoFrameWithMissingPacketDropped();
		testPacketizedOriginDistinguishedFromSinglePacketFrame();
		testVideoPerPacketSFrame();
		testVideoMixedRunLengthsInOneTimestamp();
		testVideoTwoSsrcsInterleaveIndependently();
		testStaleTimestampOnSameStreamIsDropped();
		testBatchSpanningTwoSsrcsDecryptsEachUnderItsOwnKey();
		testVideoPerStreamStalenessWatermark();
		testAudioTwoSsrcsInterleaveIndependently();
		testVideoStreamTableIsBounded();
		testSFrameWithRtxRetransmission();
		testUnresolvableGroupIsCapped();
		testFrameSpanningPastTheGroupCapIsRefused();
		testEvictedStreamResumesPastAFieldWrap();
		testTwoSsrcsWithRatchetingBothDecode();
		testIdleStreamResumesWithoutPerSsrcDerivation();
		testIdleStreamResumesWithPerSsrcDerivation();
		testStreamOpeningARatchetPeriodOnReturnDecodes();
		testStreamMapBoundedByMalformedPackets();
		testBadPacketsFiltered();
		testDescriptorByteEncoding();
		testBadDescriptorBitsIgnored();
		testReservedDescriptorBitsIgnored();
		testAudioBadDescriptorIgnored();
		testHeaderExtensionsAccounted();
		testCsrcAccounted();
		testCsrcAndExtensionAccounted();
		testMultipleHeaderExtensionsAccounted();
		testPaddingStripped();
		testPaddingOnlyPacketDoesNotBreakFrame();
		testInvalidPaddingDropped();
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
}

#endif // RTC_ENABLE_MEDIA
