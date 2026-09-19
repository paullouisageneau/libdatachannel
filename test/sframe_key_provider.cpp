/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "impl/sframeutility.hpp"
#include "test.hpp"

#include <atomic>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

namespace {

const uint16_t kSuite = 0x04; // AES-128-GCM
const uint32_t kSsrc = 0x1234ABCD;
const uint8_t kRatchetStepBits = 4;
const uint64_t kGeneration = 1;
const uint64_t kOtherGeneration = 2;

void expect(bool condition, const string &message) {
	if (!condition)
		throw std::runtime_error(message);
}

binary masterKey(uint8_t seed = 0x55) {
	return binary(16, std::byte(seed));
}

// Hands back the master key for the generation and nothing else. The per-SSRC derivation
// and the ratchet walk are the library's job -- draft-ietf-avtcore-rtp-sframe Section 8
// derives per-SSRC once and then ratchets that key, so a provider that pre-ratchets would
// put the two sides on different chains.
class MasterKeyProvider final : public SFrameKeyProvider {
public:
	bool usePerSSRCDerivation() const override { return true; }
	explicit MasterKeyProvider(binary key = masterKey()) : mKey(std::move(key)) {}

	uint8_t ratchetStepBits() const override { return kRatchetStepBits; }

	std::optional<SFrameKeyDetails> getKeyDetails(uint64_t keyGeneration) override {
		mRequested.insert(keyGeneration);
		if (mUnknown)
			return std::nullopt;
		return SFrameKeyDetails{kSuite, mKey, 0, SFrameKeyUse::Decrypt};
	}

	void setKey(binary key) { mKey = std::move(key); }
	void setUnknown(bool unknown) { mUnknown = unknown; }
	const std::set<uint64_t> &requested() const { return mRequested; }

private:
	binary mKey;
	bool mUnknown = false;
	std::set<uint64_t> mRequested;
};

// Holds two generations under different master keys, as a provider does across a re-key.
class TwoGenerationProvider final : public SFrameKeyProvider {
public:
	bool usePerSSRCDerivation() const override { return true; }

	uint8_t ratchetStepBits() const override { return kRatchetStepBits; }

	std::optional<SFrameKeyDetails> getKeyDetails(uint64_t keyGeneration) override {
		if (keyGeneration == kGeneration)
			return SFrameKeyDetails{kSuite, masterKey(), 0, SFrameKeyUse::Decrypt};
		if (keyGeneration == kOtherGeneration)
			return SFrameKeyDetails{kSuite, masterKey(0x22), 0, SFrameKeyUse::Decrypt};
		return std::nullopt;
	}
};

// A sender at a given ratchet step, built the way SFrameRtpPacketizer does it: derive the
// per-SSRC key once from the master key, then ratchet that key forward.
SFrameKeyDetails senderKeyAtStep(uint64_t step, const binary &master = masterKey()) {
	auto key = impl::SFrameUtility::DeriveSSRCKey(kSsrc, master, kSuite);
	for (uint64_t i = 0; i < step; ++i)
		key = impl::SFrameUtility::RatchetKey(kSuite, key);

	return SFrameKeyDetails{kSuite, std::move(key), 0, SFrameKeyUse::Encrypt};
}

binary payload(uint8_t seed) { return binary(64, std::byte(seed)); }

// Encrypts one frame as a sender sitting at `step` would.
message_ptr encodeAtStep(uint64_t step, const binary &plaintext,
                         const binary &master = masterKey()) {
	SFrameEncoder encoder(
	    SFrameConfig{senderKeyAtStep(step, master), kGeneration, step, kRatchetStepBits, 0});
	return encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
}

// As above, for a sender on a key generation other than the default.
message_ptr encodeAtGeneration(uint64_t generation, uint64_t step, const binary &plaintext,
                               const binary &master) {
	SFrameEncoder encoder(
	    SFrameConfig{senderKeyAtStep(step, master), generation, step, kRatchetStepBits, 0});
	return encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
}

bool decodes(SFrameDecoder &decoder, const message_ptr &frame,
             const binary &plaintext) {
	try {
		auto out = decoder.decodeFrame(frame, {});
		return binary((out)->begin(), (out)->end()) == plaintext;
	} catch (const std::exception &) {
		return false;
	}
}

// The decoder walks the per-SSRC chain to whatever step the KID names, from a provider
// that only ever supplies the master key.
void testDecoderFollowsRatchetSteps() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	for (uint64_t step = 0; step < 6; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(0x10 + step));
		expect(decodes(decoder, encodeAtStep(step, plaintext), plaintext),
		       "step " + to_string(step) + " did not decode");
	}

	// The provider is asked about a key generation, never a ratchet step, so following the
	// chain must not change what it is asked for. That the steps really advanced is proven
	// by the decodes above, each of which used a different chain position.
	expect(provider->requested().size() == 1,
	       "the provider was consulted for " + to_string(provider->requested().size()) +
	           " key generations across six ratchet steps, so a ratchet escaped its field");
}

// A receiver joining a session already under way gets its first frame from wherever that
// session has reached, so the chain is allowed one fast-forward to catch up.
void testJoinerCatchesUpOnFirstFrame() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto joined = payload(0x21);
	expect(decodes(decoder, encodeAtStep(12, joined), joined),
	       "a joiner's first frame twelve steps in did not decode");

	// And it keeps up from there
	const auto next = payload(0x22);
	expect(decodes(decoder, encodeAtStep(13, next), next),
	       "the following step did not decode after catching up");
}

// Once a frame has authenticated, the catch-up allowance is spent: a receiver that is
// keeping up should only ever be a ratchet or two behind, and a large jump after that is
// a forged step rather than a real one.
void testLargeJumpRefusedAfterCatchUp() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto first = payload(0x25);
	expect(decodes(decoder, encodeAtStep(0, first), first), "step 0 did not decode");

	// Within the steady-state window
	const auto near = payload(0x26);
	expect(decodes(decoder, encodeAtStep(2, near), near),
	       "a frame two steps ahead did not decode");

	// Far beyond it
	const auto far = payload(0x27);
	expect(!decodes(decoder, encodeAtStep(12, far), far),
	       "a distant step was accepted after the chain was established");
}

// An unauthenticated frame must not move the chain. Otherwise one forged frame naming a
// distant step drags the head past the real sender, and with no walk backwards every
// subsequent genuine frame is refused -- a permanent denial of service from one packet.
void testForgedStepDoesNotPoisonChain() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto plaintext = payload(0x28);

	// A frame that names a plausible step but is encrypted under the wrong key
	auto forged = encodeAtStep(9, plaintext, masterKey(0x99));
	expect(!decodes(decoder, forged, plaintext), "a frame under the wrong key authenticated");

	// The genuine stream, still near the start, must be unaffected
	const auto genuine = payload(0x29);
	expect(decodes(decoder, encodeAtStep(1, genuine), genuine),
	       "the forged frame advanced the chain and locked out the real sender");
}

// Frames in flight when the sender ratchets arrive after the first frame of the new step.
// The sender was sending at the old step beforehand, so the receiver has its key cached and
// the straggler still decodes; the chain is not dragged backwards by it.
void testDecoderAcceptsLateStep() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto atStep2 = payload(0x31);
	// Encoded before the ratchet, delivered after it
	auto straggler = encodeAtStep(2, atStep2);

	for (uint64_t step = 0; step <= 3; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(0x40 + step));
		expect(decodes(decoder, encodeAtStep(step, plaintext), plaintext),
		       "step " + to_string(step) + " did not decode");
	}

	expect(decodes(decoder, straggler, atStep2),
	       "a frame left over from the previous step did not decode after the ratchet");

	// And the chain has not been dragged backwards by the straggler
	const auto next = payload(0x33);
	expect(decodes(decoder, encodeAtStep(4, next), next),
	       "step 4 did not decode after a late step 2 frame");
}

// A step that was never observed cannot be served after the chain has moved past it: there
// is no walk backwards, by design, so the cache is the only thing holding old steps.
void testUnseenOlderStepRefused() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto joined = payload(0x35);
	expect(decodes(decoder, encodeAtStep(6, joined), joined), "the joining frame did not decode");

	// Step 3 was skipped over during catch-up and never derived
	const auto skipped = payload(0x36);
	expect(!decodes(decoder, encodeAtStep(3, skipped), skipped),
	       "a never-seen older step was served, so something walked the chain backwards");
}

// The provider is the only thing that knows a KID: nullopt must refuse the frame rather
// than fall back to any default key material.
void testUnknownKidRefused() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto plaintext = payload(0x41);
	provider->setUnknown(true);
	expect(!decodes(decoder, encodeAtStep(0, plaintext), plaintext),
	       "a frame decoded even though the provider reported the KID as unknown");

	provider->setUnknown(false);
	expect(decodes(decoder, encodeAtStep(0, plaintext), plaintext),
	       "the frame did not decode once the provider knew the KID");
}

// Re-keying a KID in place has to take effect: the cached chain came from the old master
// key and must not outlive it.
void testProviderRekeyTakesEffect() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto first = payload(0x51);
	expect(decodes(decoder, encodeAtStep(0, first), first), "the original key did not decode");

	const auto rekeyed = masterKey(0x66);
	provider->setKey(rekeyed);

	const auto second = payload(0x52);
	expect(decodes(decoder, encodeAtStep(0, second, rekeyed), second),
	       "the decoder kept using the old master key after a re-key");

	// And a frame under the retired key no longer decodes
	expect(!decodes(decoder, encodeAtStep(0, first), first),
	       "a frame under the retired master key still decoded");
}

// The ratchet step comes off the wire, so a peer naming a distant step must not be able to
// charge the receiver an unbounded number of HKDF chains.
void testForwardRatchetIsBounded() {
	// A wide ratchet field lets a peer name a step far beyond any real advance.
	const uint8_t wideBits = 32;
	const uint64_t farStep = 100000;

	auto key = impl::SFrameUtility::DeriveSSRCKey(kSsrc, masterKey(), kSuite);
	for (uint64_t i = 0; i < farStep; ++i)
		key = impl::SFrameUtility::RatchetKey(kSuite, key);

	SFrameKeyDetails senderKey{kSuite, std::move(key), 0, SFrameKeyUse::Encrypt};
	SFrameEncoder encoder(SFrameConfig{senderKey, kGeneration, farStep, wideBits, 0});
	const auto plaintext = payload(0x61);
	auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});

	class WideProvider final : public SFrameKeyProvider {
	public:
		bool usePerSSRCDerivation() const override { return true; }
		uint8_t ratchetStepBits() const override { return wideBits; }
		std::optional<SFrameKeyDetails> getKeyDetails(uint64_t) override {
			return SFrameKeyDetails{kSuite, masterKey(), 0, SFrameKeyUse::Decrypt};
		}
	};

	auto provider = std::make_shared<WideProvider>();
	SFrameDecoder decoder(provider, kSsrc);
	expect(!decodes(decoder, frame, plaintext),
	       "the decoder walked an unbounded ratchet chain for a distant step");
}

// Both sides must agree on R: it is what separates the ratchet step from the key
// generation, so a mismatch silently reads the KID as naming a different step.
void testRatchetStepBitsMismatchFails() {
	class MismatchedProvider final : public SFrameKeyProvider {
	public:
		bool usePerSSRCDerivation() const override { return true; }
		// The sender used 4 bits; this receiver was provisioned with 8.
		uint8_t ratchetStepBits() const override { return 8; }
		std::optional<SFrameKeyDetails> getKeyDetails(uint64_t) override {
			return SFrameKeyDetails{kSuite, masterKey(), 0, SFrameKeyUse::Decrypt};
		}
	};

	auto provider = std::make_shared<MismatchedProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	const auto plaintext = payload(0x71);
	// Step 0 agrees regardless of R, so use a step where the two readings differ.
	expect(!decodes(decoder, encodeAtStep(3, plaintext), plaintext),
	       "a frame decoded despite the two sides disagreeing about ratchetStepBits");
}

// The wire carries only the low R bits of the step, so past 2^R the same wire value names a
// different absolute step. A sender ratcheting steadily must keep decoding across that
// boundary rather than appearing to jump backwards.
void testRatchetStepWrapsWithinField() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	// Past two full turns of a 4-bit field.
	for (uint64_t step = 0; step <= 34; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(step));
		expect(decodes(decoder, encodeAtStep(step, plaintext), plaintext),
		       "step " + to_string(step) + " did not decode across the ratchet field wrap");
	}
}

// The key generation naming a chain comes off the wire unauthenticated, so a frame that
// fails to authenticate must leave the chain it named -- and every other chain -- alone.
// The stream is run past 2^R first: that is what makes a lost chain unrecoverable, since
// the wire step alone cannot say which turn of the field it belongs to.
void testForgedGenerationDoesNotResetChain() {
	auto provider = std::make_shared<TwoGenerationProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	for (uint64_t step = 0; step <= 17; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(step));
		expect(decodes(decoder, encodeAtGeneration(kGeneration, step, plaintext, masterKey()),
		               plaintext),
		       "step " + to_string(step) + " did not decode");
	}

	// Names the other generation the provider knows, under a key it does not.
	const auto forgedText = payload(0xA1);
	auto forged = encodeAtGeneration(kOtherGeneration, 17, forgedText, masterKey(0x99));
	expect(!decodes(decoder, forged, forgedText), "a frame under an unknown key authenticated");

	const auto genuine = payload(0xA2);
	expect(decodes(decoder, encodeAtGeneration(kGeneration, 18, genuine, masterKey()), genuine),
	       "one forged frame naming another key generation tore down the live chain");
}

// A provider re-keying a generation in place starts a new chain at step 0, and the first
// frame on it must decode straight away -- the old chain's position says nothing about
// where the new one starts.
void testRekeyAtNonZeroStepDecodesImmediately() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider, kSsrc);

	for (uint64_t step = 0; step <= 18; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(step));
		expect(decodes(decoder, encodeAtStep(step, plaintext), plaintext),
		       "step " + to_string(step) + " did not decode");
	}

	const auto rekeyed = masterKey(0x77);
	provider->setKey(rekeyed);

	const auto first = payload(0xB1);
	expect(decodes(decoder, encodeAtStep(0, first, rekeyed), first),
	       "the first frame after a re-key was read against the old chain's step");
}

// Without an SSRC the decoder pairs with SFrameEncoder directly and applies no per-SSRC
// derivation, so the provider's key is used as-is.
void testDecoderWithoutSsrcSkipsPerSsrcDerivation() {
	auto provider = std::make_shared<MasterKeyProvider>();
	SFrameDecoder decoder(provider); // no SSRC

	const auto plaintext = payload(0x81);
	SFrameEncoder encoder(
	    SFrameConfig{SFrameKeyDetails{kSuite, masterKey(), 0, SFrameKeyUse::Encrypt},
	                 kGeneration, 0, kRatchetStepBits, 0});
	auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});

	expect(decodes(decoder, frame, plaintext),
	       "an unbound decoder did not decode a frame encrypted with the provider's key");

	// A per-SSRC encrypted frame must not decode on an unbound decoder
	const auto ssrcFrame = encodeAtStep(0, plaintext);
	expect(!decodes(decoder, ssrcFrame, plaintext),
	       "an unbound decoder decoded a per-SSRC frame, so the derivation was applied anyway");
}

} // namespace

TestResult test_sframe_key_provider() {
	InitLogger(LogLevel::Warning);
	try {
		testDecoderFollowsRatchetSteps();
		testJoinerCatchesUpOnFirstFrame();
		testLargeJumpRefusedAfterCatchUp();
		testForgedStepDoesNotPoisonChain();
		testDecoderAcceptsLateStep();
		testUnseenOlderStepRefused();
		testUnknownKidRefused();
		testProviderRekeyTakesEffect();
		testRatchetStepWrapsWithinField();
		testForgedGenerationDoesNotResetChain();
		testRekeyAtNonZeroStepDecodesImmediately();
		testForwardRatchetIsBounded();
		testRatchetStepBitsMismatchFails();
		testDecoderWithoutSsrcSkipsPerSsrcDerivation();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

#else // RTC_ENABLE_MEDIA

TestResult test_sframe_key_provider() { return TestResult(true); }

#endif
