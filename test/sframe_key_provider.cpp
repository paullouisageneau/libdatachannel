/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframecodec.hpp"
#include "impl/sframeutility.hpp"
#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
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

const uint16_t kSuite = 0x04; // AES-128-GCM
const SSRC kSsrc = 0x1234ABCD;
const uint8_t kRatchetStepBits = 4;
const uint64_t kGeneration = 1;
const uint64_t kOtherGeneration = 2;

// Full KIDs at step 0, as a key management system would issue them.
const uint64_t kKid = kGeneration << kRatchetStepBits;
const uint64_t kOtherKid = kOtherGeneration << kRatchetStepBits;

void expect(bool condition, const string &message) {
	if (!condition)
		throw std::runtime_error(message);
}

// An exception leaving a std::thread body calls std::terminate, which takes the binary down and
// with it main()'s tally -- so a single assertion failure on a worker thread would discard the
// verdict for every test after it, in every file. Bodies record instead, and the main thread
// rethrows after joining. expect() and encodeFrame() both throw, so every thread body here needs
// this.
class ThreadFailure {
public:
	void capture(const std::exception &e) {
		std::lock_guard<std::mutex> lock(mMutex);
		if (mWhat.empty())
			mWhat = e.what();
	}

	void rethrowIfAny() const {
		std::lock_guard<std::mutex> lock(mMutex);
		if (!mWhat.empty())
			throw std::runtime_error("on a worker thread: " + mWhat);
	}

private:
	mutable std::mutex mMutex;
	string mWhat;
};

template <typename F> void guarded(ThreadFailure &failures, F &&body) {
	try {
		body();
	} catch (const std::exception &e) {
		failures.capture(e);
	}
}

binary masterKey(uint8_t seed = 0x55) { return binary(16, std::byte(seed)); }

// The plain receive provider, holding one generation.
shared_ptr<SFrameReceiveKeyProvider> receiveProvider(binary key = masterKey(),
                                                     uint64_t kid = kKid) {
	auto provider = std::make_shared<SFrameReceiveKeyProvider>(kSuite, kRatchetStepBits,
	                                                           /*perSsrcDerivation=*/true);
	provider->addKey(kid, SFrameReceiveKey{std::move(key)});
	return provider;
}

// Records which generations were asked for, so a test can tell the decoder really consulted the
// provider rather than serving the frame from its own cache.
class RecordingKeyProvider final : public SFrameReceiveKeyProvider {
public:
	RecordingKeyProvider() : SFrameReceiveKeyProvider(kSuite, kRatchetStepBits, true) {}

	optional<SFrameReceiveKey> receiveKey(uint64_t kid) const override {
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mRequested.insert(impl::sframe::KeyGenerationFromKid(kid, kRatchetStepBits));
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

// The key a sender sitting at `step` holds: draft-ietf-avtcore-rtp-sframe Section 8 derives per
// SSRC once and then ratchets that key, so the two sides only agree past step 0 by walking the
// same chain. Built here rather than by the encoder, which is given it as-is because these tests
// construct the encoder without an SSRC.
binary senderKeyAtStep(uint64_t step, const binary &master = masterKey()) {
	auto key = impl::sframe::DeriveSSRCKey(kSsrc, master, kSuite);
	for (uint64_t i = 0; i < step; ++i)
		key = impl::sframe::RatchetKey(kSuite, key);

	return key;
}

binary payload(uint8_t seed) { return binary(64, std::byte(seed)); }

// Encrypts one frame as a sender on `generation` sitting at `step` would.
message_ptr encodeAtGeneration(uint64_t generation, uint64_t step, const binary &plaintext,
                               const binary &master) {
	auto provider = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, kRatchetStepBits, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
	    SFrameSendKey{senderKeyAtStep(step, master),
	                  impl::sframe::MakeKid(generation, step, kRatchetStepBits)});

	impl::SFrameEncoder encoder(provider); // no SSRC, so the key is used as given
	return encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
}

message_ptr encodeAtStep(uint64_t step, const binary &plaintext,
                         const binary &master = masterKey()) {
	return encodeAtGeneration(kGeneration, step, plaintext, master);
}

// The three ways the decoder declines a frame: the provider held no key for the KID, the wire
// step named a chain position it will not walk to, and the tag did not verify. Every one of them
// is a std::runtime_error -- and so is every crypto backend failure and every internal error --
// so they are named rather than caught by type. A test asserting a frame is rejected has to fail
// if the frame never reached the cipher at all.
const char *const kNoKeyForKid = "No SFrame key for KID";
const char *const kRefusedStep = "SFrame ratchet step";
// Two spellings, because a suite that authenticates with HMAC reports it differently from GCM.
const char *const kAuthFailed = "Authentication failed";
const char *const kGcmAuthFailed = "AES-GCM authentication failed";

bool mentions(const string &reason, const char *what) { return reason.find(what) != string::npos; }

bool isAuthFailure(const string &reason) {
	return mentions(reason, kAuthFailed) || mentions(reason, kGcmAuthFailed);
}

bool isRefusal(const string &reason) {
	return isAuthFailure(reason) || mentions(reason, kNoKeyForKid) ||
	       mentions(reason, kRefusedStep);
}

// Why the decoder declined the frame, or empty when it decoded. Anything that is not a refusal
// propagates: swallowing it would let a genuine defect read as the decoder doing its job.
string refusalReason(impl::SFrameDecoder &decoder, const message_ptr &frame,
                     const binary &plaintext) {
	binary out;
	try {
		auto decoded = decoder.decodeFrame(frame, {});
		out = binary((decoded)->begin(), (decoded)->end());
	} catch (const std::exception &e) {
		const string reason = e.what();
		if (!isRefusal(reason))
			throw;

		return reason;
	}

	// Returning at all means the frame authenticated, so a plaintext that does not match is a
	// defect rather than a refusal and must never be reported as one.
	expect(out == plaintext, "a frame authenticated but came back as different plaintext");
	return {};
}

bool decodes(impl::SFrameDecoder &decoder, const message_ptr &frame, const binary &plaintext) {
	return refusalReason(decoder, frame, plaintext).empty();
}

// A provider that answers with key material too short to use, as a broken or hostile override of
// receiveKey() would. SFrameDecoder::selectChain() refuses it before deriving anything, and that
// refusal is not one of the three above: the key never reached the cipher, so nothing was
// authenticated or declined.
class ShortKeyProvider final : public SFrameReceiveKeyProvider {
public:
	ShortKeyProvider() : SFrameReceiveKeyProvider(kSuite, kRatchetStepBits, true) {}

	optional<SFrameReceiveKey> receiveKey(uint64_t) const override {
		return SFrameReceiveKey{binary(impl::sframe::MinBaseKeySize() - 1, std::byte(0x11))};
	}
};

// An override handing the decoder unusable key material must not look like the decoder declining a
// frame. Every negative assertion in this file is an expect(!decodes(...)), so anything that fails
// for an unrelated reason and still reads as a refusal would satisfy all of them at once.
void testUnusableKeyIsNotARefusal() {
	auto provider = std::make_shared<ShortKeyProvider>();
	impl::SFrameDecoder decoder(provider, kSsrc);

	const auto plaintext = payload(0xF8);
	auto frame = encodeAtStep(0, plaintext);

	bool propagated = false;
	try {
		decodes(decoder, frame, plaintext);
	} catch (const std::invalid_argument &) {
		propagated = true;
	}
	expect(propagated, "a base key the decoder could not use was reported as a refusal, so a "
	                   "rejection assertion would be satisfied by a broken provider");
}

// A frame served from the derived-key cache still proves the step it was decoded at.
//
// The proven step is what lets a stream returning from idle walk to where its siblings already are
// without being charged as speculative. It is recorded per master key on the m-line-wide limiter,
// while the derived-key cache is per decoder -- so the two can be driven apart, and a cache hit
// arriving after the limiter has evicted that master key takes the insert path rather than the
// update path. The update path hides a missing step behind a std::max; the insert path writes it
// verbatim, so a cache hit that reports no step silently resets the m-line's reference to zero.
void testCachedFrameStillProvesItsStep() {
	auto limiter = std::make_shared<impl::SFrameDecoder::CatchUpLimiter>();
	const uint64_t provenAt = 5;

	// The decoder under test: one SSRC, one generation, decoding at a non-zero step.
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc, limiter);

	const auto plaintext = payload(0x5A);
	auto frame = encodeAtStep(provenAt, plaintext);
	expect(decodes(decoder, frame, plaintext), "the first frame did not decode");
	expect(limiter->provenStep(masterKey()) == provenAt,
	       "the first decode did not record its step, so this test cannot detect losing it");

	// Push that master key out of the limiter. Each of these authenticates under its own master
	// key on its own decoder, so the shared limiter fills while the decoder above keeps its
	// derived-key cache untouched -- which is the state that makes a cache hit take the insert
	// path. MaxProvenSteps is private, so this drives well past it rather than relying on it.
	for (uint8_t i = 0; i < 12; ++i) {
		const binary other = masterKey(uint8_t(0x80 + i));
		auto otherProvider = receiveProvider(other);
		impl::SFrameDecoder otherDecoder(otherProvider, kSsrc, limiter);
		const auto otherPlain = payload(uint8_t(0xB0 + i));
		expect(decodes(otherDecoder, encodeAtStep(0, otherPlain, other), otherPlain),
		       "a filler generation did not decode");
	}
	expect(limiter->provenStep(masterKey()) == 0,
	       "the filler generations did not evict the original master key, so the cache hit below "
	       "would take the update path and prove nothing");

	// The same frame again. It is served from the decoder's cache, and committing it has to put
	// the m-line's reference back where it belongs rather than at zero.
	expect(decodes(decoder, frame, plaintext), "the cached frame did not decode");
	expect(limiter->provenStep(masterKey()) == provenAt,
	       "a frame served from the derived-key cache recorded step 0 as proven: a sibling "
	       "returning from idle would be refused the walk it is owed");
}

// A paused stream is walked forward by its siblings, so it does not pay for every period it
// missed on its first frame back.
//
// Nothing about the wire changes -- it emits the same KID either way -- so the observable is where
// the work happened: the idle encoder's ratchetCount() must already be at the shared step *before*
// it encodes anything. Without the sweep it sits at its old count until its next frame.
void testIdleEncoderIsWalkedForwardBySiblings() {
	auto provider = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, kRatchetStepBits, /*ratchetPeriod=*/1, /*perSsrcDerivation=*/true,
	    SFrameSendKey{masterKey(), kKid});
	const SSRC busy = 0xAA00AA00;
	const SSRC idle = 0xBB00BB00;

	auto busyEncoder = impl::SFrameEncoder::ForTrack(provider, busy);
	auto idleEncoder = impl::SFrameEncoder::ForTrack(provider, idle);
	expect(idleEncoder->ratchetCount() == 0, "a fresh encoder should start at step 0");

	// Only the busy stream sends, across several periods.
	const auto plaintext = payload(0x6A);
	for (int round = 0; round < 3; ++round) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		busyEncoder->encodeFrame(make_message(binary(plaintext), Message::Binary), {});
	}

	const uint64_t shared = busyEncoder->ratchetCount();
	expect(shared >= 3, "the busy stream should have advanced at least one step per period, got " +
	                        to_string(shared));

	// The idle stream has sent nothing, yet must already be level with its sibling.
	expect(idleEncoder->ratchetCount() == shared,
	       "the idle encoder is at step " + to_string(idleEncoder->ratchetCount()) + " while its " +
	           "sibling is at " + to_string(shared) +
	           ": it was not walked forward and will pay the whole catch-up on its next frame");

	// And it still encodes at the shared step, so spreading the work changed nothing observable.
	auto framed = idleEncoder->encodeFrame(make_message(binary(plaintext), Message::Binary), {});
	expect(framed != nullptr, "the idle encoder failed to encode after being walked forward");
	expect(idleEncoder->ratchetCount() == shared,
	       "encoding after the sweep advanced the step again, so the counter was disturbed");
}

// The decoder walks the per-SSRC chain to whatever step the KID names, from a provider
// that only ever supplies the master key.
void testDecoderFollowsRatchetSteps() {
	auto provider = std::make_shared<RecordingKeyProvider>();
	provider->addKey(kKid, SFrameReceiveKey{masterKey()});
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

	const auto first = payload(0x25);
	expect(decodes(decoder, encodeAtStep(0, first), first), "step 0 did not decode");

	// Within the steady-state window
	const auto near = payload(0x26);
	expect(decodes(decoder, encodeAtStep(2, near), near), "a frame two steps ahead did not decode");

	// Far beyond it
	const auto far = payload(0x27);
	expect(!decodes(decoder, encodeAtStep(12, far), far),
	       "a distant step was accepted after the chain was established");
}

// An unauthenticated frame must not move the chain. Otherwise one forged frame naming a
// distant step drags the head past the real sender, and with no walk backwards every
// subsequent genuine frame is refused -- a permanent denial of service from one packet.
void testForgedStepDoesNotPoisonChain() {
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

	const auto plaintext = payload(0x41);
	provider->removeKey(kKid);
	expect(!decodes(decoder, encodeAtStep(0, plaintext), plaintext),
	       "a frame decoded even though the provider reported the KID as unknown");

	provider->addKey(kKid, SFrameReceiveKey{masterKey()});
	expect(decodes(decoder, encodeAtStep(0, plaintext), plaintext),
	       "the frame did not decode once the provider knew the KID");
}

// Re-keying a KID in place has to take effect: the cached chain came from the old master
// key and must not outlive it.
void testProviderRekeyTakesEffect() {
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

	const auto first = payload(0x51);
	expect(decodes(decoder, encodeAtStep(0, first), first), "the original key did not decode");

	const auto rekeyed = masterKey(0x66);
	provider->addKey(kKid, SFrameReceiveKey{rekeyed});

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

	auto key = impl::sframe::DeriveSSRCKey(kSsrc, masterKey(), kSuite);
	for (uint64_t i = 0; i < farStep; ++i)
		key = impl::sframe::RatchetKey(kSuite, key);

	auto sender = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, wideBits, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
	    SFrameSendKey{std::move(key), impl::sframe::MakeKid(kGeneration, farStep, wideBits)});
	impl::SFrameEncoder encoder(sender);
	const auto plaintext = payload(0x61);
	auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});

	auto provider = std::make_shared<SFrameReceiveKeyProvider>(kSuite, wideBits,
	                                                           /*perSsrcDerivation=*/true);
	provider->addKey(kGeneration << wideBits, SFrameReceiveKey{masterKey()});
	impl::SFrameDecoder decoder(provider, kSsrc);
	expect(!decodes(decoder, frame, plaintext),
	       "the decoder walked an unbounded ratchet chain for a distant step");
}

// Both sides must agree on R: it is what separates the ratchet step from the key generation, so a
// mismatch reads the KID as naming a different generation *and* a different step. Both halves are
// pinned, because either one alone can be the thing that refuses the frame and a test satisfied by
// the first would pass with the step arithmetic broken.
void testRatchetStepBitsMismatchFails() {
	// The sender used 4 bits throughout; these receivers were provisioned with 8. Step 3 with
	// generation 1 makes the whole KID 0x13, which a receiver on 8 bits reads as generation 0 at
	// step 19.
	const uint64_t senderStep = 3;
	const uint64_t sentKid = impl::sframe::MakeKid(kGeneration, senderStep, kRatchetStepBits);
	expect(impl::sframe::KeyGenerationFromKid(sentKid, 8) != kGeneration &&
	           impl::sframe::RatchetStepFromKid(sentKid, 8) != senderStep,
	       "the two readings of the KID have to differ in both halves, or this tests nothing");

	// Read against the wrong R, the generation is one the provider never registered, so the lookup
	// is what refuses the frame.
	{
		auto provider = std::make_shared<SFrameReceiveKeyProvider>(kSuite, /*ratchetStepBits=*/8,
		                                                           /*perSsrcDerivation=*/true);
		provider->addKey(kGeneration << 8, SFrameReceiveKey{masterKey()});
		impl::SFrameDecoder decoder(provider, kSsrc);

		const auto plaintext = payload(0x71);
		const auto reason = refusalReason(decoder, encodeAtStep(senderStep, plaintext), plaintext);
		expect(mentions(reason, kNoKeyForKid),
		       "a frame decoded despite the two sides disagreeing about ratchetStepBits: " +
		           reason);
	}

	// And with the key registered under the generation the mismatched reading lands on, so the
	// lookup succeeds and only the step disagrees: the receiver walks its chain to 19 while the
	// sender is at 3, and the frame must fail to authenticate rather than be read at the wrong
	// step and silently produce rubbish.
	{
		auto provider = std::make_shared<SFrameReceiveKeyProvider>(kSuite, /*ratchetStepBits=*/8,
		                                                           /*perSsrcDerivation=*/true);
		provider->addKey(sentKid, SFrameReceiveKey{masterKey()});
		impl::SFrameDecoder decoder(provider, kSsrc);

		const auto plaintext = payload(0x72);
		const auto reason = refusalReason(decoder, encodeAtStep(senderStep, plaintext), plaintext);
		expect(isAuthFailure(reason),
		       "a mismatched ratchet step was not caught by the tag, the decoder said: " + reason);
	}

	// Step 0 is the one place the step halves agree, and the generation halves still do not, so
	// even the first frame of a session does not slip through an R mismatch.
	{
		auto provider = std::make_shared<SFrameReceiveKeyProvider>(kSuite, /*ratchetStepBits=*/8,
		                                                           /*perSsrcDerivation=*/true);
		provider->addKey(kGeneration << 8, SFrameReceiveKey{masterKey()});
		impl::SFrameDecoder decoder(provider, kSsrc);

		const auto plaintext = payload(0x73);
		expect(!decodes(decoder, encodeAtStep(0, plaintext), plaintext),
		       "the first frame of a session decoded across an R mismatch");
	}
}

// The wire carries only the low R bits of the step, so past 2^R the same wire value names a
// different absolute step. A sender ratcheting steadily must keep decoding across that
// boundary rather than appearing to jump backwards.
void testRatchetStepWrapsWithinField() {
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	provider->addKey(kOtherKid, SFrameReceiveKey{masterKey(0x22)});
	impl::SFrameDecoder decoder(provider, kSsrc);

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
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

	for (uint64_t step = 0; step <= 18; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(step));
		expect(decodes(decoder, encodeAtStep(step, plaintext), plaintext),
		       "step " + to_string(step) + " did not decode");
	}

	const auto rekeyed = masterKey(0x77);
	provider->addKey(kKid, SFrameReceiveKey{rekeyed});

	const auto first = payload(0xB1);
	expect(decodes(decoder, encodeAtStep(0, first, rekeyed), first),
	       "the first frame after a re-key was read against the old chain's step");
}

// Without an SSRC the decoder pairs with impl::SFrameEncoder directly and applies no per-SSRC
// derivation, so the provider's key is used as-is.
void testDecoderWithoutSsrcSkipsPerSsrcDerivation() {
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider); // no SSRC

	const auto plaintext = payload(0x81);
	auto sender = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, kRatchetStepBits, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
	    SFrameSendKey{masterKey(), kKid});
	impl::SFrameEncoder encoder(sender);
	auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});

	expect(decodes(decoder, frame, plaintext),
	       "an unbound decoder did not decode a frame encrypted with the provider's key");

	// A per-SSRC encrypted frame must not decode on an unbound decoder
	const auto ssrcFrame = encodeAtStep(0, plaintext);
	expect(!decodes(decoder, ssrcFrame, plaintext),
	       "an unbound decoder decoded a per-SSRC frame, so the derivation was applied anyway");
}

} // namespace

// A joiner's first frame is allowed one long walk, bounded by MaxInitialRatchet. The bound
// itself must be reachable and one past it refused -- the existing joiner test sits well
// inside the window, so an off-by-one either way would not show up there. A wide ratchet
// field is needed for the steps to be distinguishable at all: with R=4 the wire step wraps
// every 16 and the decoder reads it relative to the chain head.
void testInitialCatchUpBoundary() {
	const uint8_t wideBits = 16;

	auto wideProvider = [&]() {
		auto p = std::make_shared<SFrameReceiveKeyProvider>(kSuite, wideBits,
		                                                    /*perSsrcDerivation=*/true);
		p->addKey(kGeneration << wideBits, SFrameReceiveKey{masterKey()});
		return p;
	};

	// A sender sitting at `step`, with the wide ratchet field.
	auto encodeWideAtStep = [&](uint64_t step, const binary &plaintext) {
		auto key = impl::sframe::DeriveSSRCKey(kSsrc, masterKey(), kSuite);
		for (uint64_t i = 0; i < step; ++i)
			key = impl::sframe::RatchetKey(kSuite, key);

		auto sender = std::make_shared<SFrameSendKeyProvider>(
		    kSuite, wideBits, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
		    SFrameSendKey{std::move(key), impl::sframe::MakeKid(kGeneration, step, wideBits)});
		impl::SFrameEncoder encoder(sender);
		return encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
	};

	// The bound is a private constant, so it is restated here: if it changes, this test has to
	// be updated deliberately rather than silently tracking it.
	const uint64_t maxInitialRatchet = 2048;

	{
		auto provider = wideProvider();
		impl::SFrameDecoder decoder(provider, kSsrc);
		const auto at = payload(0x40);
		expect(decodes(decoder, encodeWideAtStep(maxInitialRatchet, at), at),
		       "a joiner exactly at the catch-up bound was refused");
	}
	{
		auto provider = wideProvider();
		impl::SFrameDecoder decoder(provider, kSsrc);
		const auto beyond = payload(0x41);
		expect(!decodes(decoder, encodeWideAtStep(maxInitialRatchet + 1, beyond), beyond),
		       "a joiner one step past the catch-up bound was accepted");
	}
}

// The long walk is rate limited as well as bounded, so a forged step cannot force one per
// frame. A distant frame under the wrong key spends the allowance without establishing the
// chain, and a genuine frame at the same distance immediately afterwards must be refused
// rather than granted a second walk.
void testCatchUpIsRateLimited() {
	auto provider = receiveProvider();
	impl::SFrameDecoder decoder(provider, kSsrc);

	const auto forged = payload(0x44);
	expect(!decodes(decoder, encodeAtStep(12, forged, masterKey(0x99)), forged),
	       "a frame under the wrong key authenticated");

	const auto genuine = payload(0x45);
	expect(!decodes(decoder, encodeAtStep(12, genuine), genuine),
	       "a second long walk was granted within the rate limit window");

	// The steady-state window still works, so the chain is not wedged.
	const auto near = payload(0x46);
	expect(decodes(decoder, encodeAtStep(1, near), near),
	       "a nearby step was refused after the catch-up allowance was spent");
}

// One provider serving several decoders on different threads, which is what the API promises
// an implementation holding mutable state must tolerate. RecordingKeyProvider records every
// generation it is asked about, so the calls really do land on shared state.
void testProviderIsThreadSafe() {
	auto provider = std::make_shared<RecordingKeyProvider>();
	provider->addKey(kKid, SFrameReceiveKey{masterKey()});

	constexpr int kThreads = 4;
	constexpr int kFramesPerThread = 40;

	// Encrypted once up front, so the threads spend their time in the decoders.
	std::vector<std::pair<message_ptr, binary>> frames;
	for (int i = 0; i < kFramesPerThread; ++i) {
		auto plaintext = payload(static_cast<uint8_t>(0x50 + i));
		frames.emplace_back(encodeAtStep(0, plaintext), plaintext);
	}

	std::atomic<int> decoded{0};
	std::atomic<int> failed{0};
	ThreadFailure failures;
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t]() {
			guarded(failures, [&] {
				// A decoder per thread, all sharing the one provider: each holds its own chain, so
				// the provider is the only state crossed between them. Distinct SSRCs, as separate
				// tracks would have, which means only the thread on kSsrc can authenticate these
				// frames -- the rest have to fail cleanly rather than corrupt anything.
				impl::SFrameDecoder decoder(provider, kSsrc + uint32_t(t));
				for (const auto &entry : frames) {
					if (decodes(decoder, entry.first, entry.second))
						decoded++;
					else
						failed++;
				}
			});
		});
	}
	for (auto &thread : threads)
		thread.join();
	failures.rethrowIfAny();

	// Every iteration increments exactly one of the two counters, so their sum is fixed by
	// construction and asserting it says nothing. What is worth pinning is how the total splits:
	// the one thread whose SSRC matches decodes every frame, and the other three fail every frame
	// cleanly rather than decoding one or corrupting another's state.
	expect(decoded == kFramesPerThread,
	       "expected exactly the matching SSRC's frames to decode, got " +
	           std::to_string(decoded.load()));
	expect(failed == (kThreads - 1) * kFramesPerThread,
	       "expected the " + std::to_string(kThreads - 1) +
	           " mismatched SSRCs to fail every frame, got " + std::to_string(failed.load()) +
	           " failures of " + std::to_string((kThreads - 1) * kFramesPerThread));
	expect(provider->requested().count(kGeneration) == 1,
	       "the provider was never asked for the sender's key generation");
}

// A send-side provider starting on kGeneration. Rolling it is how an application rekeys.
shared_ptr<SFrameSendKeyProvider> sendProvider(binary key = masterKey()) {
	return std::make_shared<SFrameSendKeyProvider>(kSuite, kRatchetStepBits, /*ratchetPeriod=*/0,
	                                               /*perSsrcDerivation=*/true,
	                                               SFrameSendKey{std::move(key), kKid});
}

// Rekeying is a provider rotation, and the receiver follows because it looks keys up by the
// generation on the wire. The provider here answers for both generations, which is what a
// receiver must do across a rekey.
void testSendKeyRotationRekeys() {
	auto sender = sendProvider();
	impl::SFrameEncoder encoder(sender, kSsrc);

	auto receiver = receiveProvider();
	receiver->addKey(kOtherKid, SFrameReceiveKey{masterKey(0x22)});
	impl::SFrameDecoder decoder(receiver, kSsrc);

	const auto before = payload(0x60);
	auto firstFrame = encoder.encodeFrame(make_message(binary(before), Message::Binary), {});
	expect(decodes(decoder, firstFrame, before), "the pre-rekey frame did not decode");

	sender->rollKey(SFrameSendKey{masterKey(0x22), kOtherKid});

	const auto after = payload(0x61);
	auto secondFrame = encoder.encodeFrame(make_message(binary(after), Message::Binary), {});
	expect(decodes(decoder, secondFrame, after), "the post-rekey frame did not decode");

	// The generation on the wire must have moved, or the receiver could not tell the keys
	// apart and the rekey would be invisible.
	const uint64_t firstKid = impl::sframe::header::Decode(*firstFrame).kid;
	const uint64_t secondKid = impl::sframe::header::Decode(*secondFrame).kid;
	expect(impl::sframe::KeyGenerationFromKid(firstKid, kRatchetStepBits) == kGeneration,
	       "the pre-rekey KID named the wrong key generation");
	expect(impl::sframe::KeyGenerationFromKid(secondKid, kRatchetStepBits) == kOtherGeneration,
	       "the post-rekey KID did not name the new key generation");
}

// The sender moving to N+1 leaves N frames in flight, so the receiver has to hold both until
// they drain. Delivering the N frame *after* the N+1 frame is the case that matters, since
// that is what reordering and jitter produce.
void testRekeyOverlapDecodesInFlightFrames() {
	auto sender = sendProvider();
	impl::SFrameEncoder encoder(sender, kSsrc);

	const auto inFlight = payload(0x64);
	auto oldFrame = encoder.encodeFrame(make_message(binary(inFlight), Message::Binary), {});

	sender->rollKey(SFrameSendKey{masterKey(0x22), kOtherKid});

	const auto fresh = payload(0x65);
	auto newFrame = encoder.encodeFrame(make_message(binary(fresh), Message::Binary), {});

	// A provider holding both generations: the new frame arrives first, the old one straggles.
	{
		auto receiver = receiveProvider();
		receiver->addKey(kOtherKid, SFrameReceiveKey{masterKey(0x22)});
		impl::SFrameDecoder decoder(receiver, kSsrc);
		expect(decodes(decoder, newFrame, fresh), "the new generation's frame did not decode");
		expect(decodes(decoder, oldFrame, inFlight),
		       "an in-flight frame under the previous generation was lost after the rekey");
	}

	// And a provider that drops the previous generation the moment it rotates loses it, which
	// is why the overlap is a requirement on the application rather than advice.
	{
		auto receiver = receiveProvider(masterKey(0x22), kOtherKid);
		impl::SFrameDecoder decoder(receiver, kSsrc);
		expect(decodes(decoder, newFrame, fresh), "the new generation's frame did not decode");
		expect(!decodes(decoder, oldFrame, inFlight),
		       "a provider answering only for the new generation decoded an old frame");
	}
}

// rollKey() accepts the KID already in force, so long as the material is new. KID lifecycle is the
// application's, and a provider cannot tell a legitimate re-derivation from a mistake without
// retaining past key material, which would work against the forward secrecy a roll exists to
// provide. The roll must actually take effect: the encoder reads the stored version per frame
// rather than comparing KIDs, so a roll that does not change the KID is still visible to it.
void testRollKeyAcceptsTheSameGeneration() {
	auto sender = sendProvider();
	impl::SFrameEncoder encoder(sender, kSsrc);

	// A frame under the original key, so the roll has something to replace.
	const auto before = payload(0x76);
	auto firstFrame = encoder.encodeFrame(make_message(binary(before), Message::Binary), {});

	// The same KID, with new material. New material always enters at ratchet step 0, which is why
	// the KID is reused as issued rather than advanced.
	sender->rollKey(SFrameSendKey{masterKey(0x77), kKid});

	const auto after = payload(0x77);
	auto secondFrame = encoder.encodeFrame(make_message(binary(after), Message::Binary), {});

	// The encoder adopted the new key: a receiver holding only the original cannot read the second
	// frame, and one holding the new material can -- both under the same KID, so nothing but the
	// key material distinguishes them.
	expect(impl::sframe::header::Decode(*firstFrame).kid ==
	           impl::sframe::header::Decode(*secondFrame).kid,
	       "the two frames should share a KID, or this is not testing a same-generation roll");
	{
		auto original = receiveProvider(masterKey(), kKid);
		impl::SFrameDecoder decoder(original, kSsrc);
		expect(decodes(decoder, firstFrame, before), "the pre-roll frame did not decode");
		expect(
		    !decodes(decoder, secondFrame, after),
		    "the post-roll frame decoded under the replaced key, so the roll did not take effect");
	}
	{
		auto rolled = receiveProvider(masterKey(0x77), kKid);
		impl::SFrameDecoder decoder(rolled, kSsrc);
		expect(decodes(decoder, secondFrame, after), "the post-roll frame did not decode");
	}

	// A different generation is accepted too, so nothing here is refusing everything.
	sender->rollKey(SFrameSendKey{masterKey(0x78), kOtherKid});
}

// A new KID is a new nonce space: the KID is an HKDF input to both the key and the salt, so the
// counter restarts rather than climbing forever. That keeps the CTR field one byte across a
// ratchet, and makes ctrStart mean "counters spent under this KID" rather than a session total.
void testRollAndRatchetResetTheCounter() {
	auto sender = sendProvider();
	impl::SFrameEncoder encoder(sender, kSsrc);

	for (int i = 0; i < 5; ++i)
		encoder.encodeFrame(make_message(payload(static_cast<uint8_t>(0x70 + i)), Message::Binary),
		                    {});
	expect(encoder.currentCounter() == 5, "the counter did not advance with the frames sent, got " +
	                                          to_string(encoder.currentCounter()));

	// A roll restarts at the new key's ctrStart.
	sender->rollKey(SFrameSendKey{masterKey(0x22), kOtherKid, /*ctrStart=*/0});
	encoder.encodeFrame(make_message(payload(0x7F), Message::Binary), {});
	expect(encoder.currentCounter() == 1,
	       "the counter did not restart on a roll, got " + to_string(encoder.currentCounter()));

	// And a non-zero ctrStart is honoured, for resuming a KID part way along. A third generation, so
	// this is not also testing the same-generation roll above.
	sender->rollKey(
	    SFrameSendKey{masterKey(0x33), uint64_t(3) << kRatchetStepBits, /*ctrStart=*/900});
	encoder.encodeFrame(make_message(payload(0x7E), Message::Binary), {});
	expect(encoder.currentCounter() == 901,
	       "a non-zero ctrStart was not honoured, got " + to_string(encoder.currentCounter()));
}

// With per-SSRC derivation off every track sends under the same base key and KID, so they share
// one derived key and must share its counter. The provider owns the one encoder and hands it to
// every track, so a second one on that key cannot exist to restart the counter and reuse nonces.
void testSharedKeyGivesEveryTrackOneEncoder() {
	auto shared = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
	    SFrameSendKey{masterKey(), /*kid=*/0});

	auto first = impl::SFrameEncoder::ForTrack(shared, kSsrc);
	auto second = impl::SFrameEncoder::ForTrack(shared, kSsrc + 1);
	expect(first && first == second, "tracks on a shared key were given different encoders");

	// With derivation on the keys differ per SSRC, so each track gets its own counter.
	auto perSsrc = sendProvider();
	auto a = impl::SFrameEncoder::ForTrack(perSsrc, kSsrc);
	auto b = impl::SFrameEncoder::ForTrack(perSsrc, kSsrc + 1);
	expect(a && b && a != b, "per-SSRC tracks were made to share one encoder");

	// But two packetizers on the *same* SSRC derive the same key, so they must share its counter as
	// well. Left unshared, each would restart at ctrStart and repeat every nonce that SSRC had
	// already sent under.
	auto again = impl::SFrameEncoder::ForTrack(perSsrc, kSsrc);
	expect(again == a, "a second encoder was built for an SSRC already in use, so it would derive "
	                   "the same key and restart the counter");

	// And it really is one counter, not two handles that happen to compare equal.
	a->encodeFrame(make_message(payload(0xD0), Message::Binary), {});
	expect(again->currentCounter() == 1,
	       "the second handle reports counter " + to_string(again->currentCounter()) +
	           " after one frame on the first, so they are not the same counter");
}

// Resuming a key needs the counter it reached, and the encoder holding it is internal, so the
// provider reports it. Only with per-SSRC derivation off, where one encoder serves the session.
void testCurrentKeyReportsTheLiveCounter() {
	auto shared = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
	    SFrameSendKey{masterKey(), /*kid=*/7, /*ctrStart=*/40});

	auto state = shared->currentKey();
	expect(state.has_value(), "no current key reported with one encoder serving the session");
	expect(state->kid == 7 && state->ctrStart == 40,
	       "the current key did not start at the KID and ctrStart it was given");

	auto encoder = impl::SFrameEncoder::ForTrack(shared, kSsrc);
	for (int i = 0; i < 3; ++i)
		encoder->encodeFrame(make_message(payload(uint8_t(0xD0 + i)), Message::Binary), {});

	state = shared->currentKey();
	expect(state->ctrStart == 43,
	       "the current key did not follow the counter, got " + to_string(state->ctrStart));

	// A roll moves both: the new KID is a fresh nonce space, so the counter restarts under it.
	shared->rollKey(SFrameSendKey{masterKey(0x44), /*kid=*/9, /*ctrStart=*/0});
	encoder->encodeFrame(make_message(payload(0xD4), Message::Binary), {});
	state = shared->currentKey();
	expect(state->kid == 9 && state->ctrStart == 1, "the current key did not follow a roll");

	// With derivation on the counter belongs to each track's derived key, so there is nothing
	// single to report.
	expect(!sendProvider()->currentKey().has_value(),
	       "a current key was reported with per-SSRC derivation on");
}

// The three fields have to describe one point on the ratchet chain, which the counter alone does
// not prove: pairing the key the provider was given with an advanced KID derives a key no receiver
// computes, so this resumes a sender from what currentKey() reported and decodes it against a
// receiver that walked the chain itself.
void testCurrentKeyResumesOnTheSameChain() {
	const uint8_t bits = 4;
	const uint64_t kid = kGeneration << bits;

	auto sender = std::make_shared<SFrameSendKeyProvider>(kSuite, bits, /*ratchetPeriod=*/1,
	                                                      /*perSsrcDerivation=*/false,
	                                                      SFrameSendKey{masterKey(), kid});
	auto encoder = impl::SFrameEncoder::ForTrack(sender, kSsrc);

	// The receiver only ever gets the key at step 0 and ratchets forward on its own.
	auto receiver = std::make_shared<SFrameReceiveKeyProvider>(kSuite, bits,
	                                                           /*perSsrcDerivation=*/false);
	receiver->addKey(kid, SFrameReceiveKey{masterKey()});
	impl::SFrameDecoder decoder(receiver, kSsrc);

	// Ratchet the sender off step 0, so a resumed sender that ignored the chain would diverge.
	std::set<uint64_t> kids;
	for (int i = 0; i < 4; ++i) {
		const auto plaintext = payload(uint8_t(0xE0 + i));
		auto frame = encoder->encodeFrame(make_message(binary(plaintext), Message::Binary), {});
		kids.insert(impl::sframe::header::Decode(*frame).kid);
		expect(decodes(decoder, frame, plaintext), "a pre-resume frame did not decode");
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
	}
	expect(kids.size() > 1, "the sender never ratcheted, so resuming was not exercised");

	// Resume from exactly what the provider reported, as an application persisting it would.
	auto state = sender->currentKey();
	expect(state.has_value(), "no current key to resume from");
	auto resumed = std::make_shared<SFrameSendKeyProvider>(kSuite, bits, /*ratchetPeriod=*/0,
	                                                       /*perSsrcDerivation=*/false, *state);
	auto resumedEncoder = impl::SFrameEncoder::ForTrack(resumed, kSsrc);

	const auto after = payload(0xEF);
	auto frame = resumedEncoder->encodeFrame(make_message(binary(after), Message::Binary), {});
	expect(impl::sframe::header::Decode(*frame).kid == state->kid,
	       "the resumed sender did not send under the KID it resumed at");
	expect(decodes(decoder, frame, after),
	       "the resumed sender was not on the receiver's chain: currentKey() did not report the "
	       "ratcheted key its KID names");
}

// Re-supplying the key already in force must do nothing. Adopting a key restarts the counter at
// ctrStart, so treating a re-supply as a roll would replay every nonce already emitted under that
// same key and KID -- the one catastrophic mistake this API can recognise without retaining past
// key material, since it only has to compare against the key it is holding. A genuine roll must
// still reset, which is the second half of this test: the no-op has to be narrow.
void testRollKeyIgnoresTheKeyAlreadyInForce() {
	// perSsrcDerivation off, so one encoder is shared and its counter is the session's -- there is
	// a single counter to make assertions about.
	auto sender = std::make_shared<SFrameSendKeyProvider>(kSuite, kRatchetStepBits,
	                                                      /*ratchetPeriod=*/0,
	                                                      /*perSsrcDerivation=*/false,
	                                                      SFrameSendKey{masterKey(), kKid});
	auto encoder = impl::SFrameEncoder::ForTrack(sender, kSsrc);

	for (int i = 0; i < 5; ++i)
		encoder->encodeFrame(make_message(payload(uint8_t(0xC0 + i)), Message::Binary), {});
	expect(encoder->currentCounter() == 5, "expected the counter at 5 after five frames, got " +
	                                           to_string(encoder->currentCounter()));

	// The same KID and the same base key, byte for byte.
	sender->rollKey(SFrameSendKey{masterKey(), kKid});

	encoder->encodeFrame(make_message(payload(0xC9), Message::Binary), {});
	expect(encoder->currentCounter() == 6,
	       "re-supplying the key already in force restarted the counter at " +
	           to_string(encoder->currentCounter()) +
	           ", replaying every nonce already emitted under it");

	// A genuine roll -- same KID, new material -- still resets, so the no-op did not disable
	// rolling.
	sender->rollKey(SFrameSendKey{masterKey(0x99), kKid});
	encoder->encodeFrame(make_message(payload(0xCA), Message::Binary), {});
	expect(encoder->currentCounter() == 1,
	       "a roll to new material under the same KID did not restart the counter, got " +
	           to_string(encoder->currentCounter()));
}

// KID lifecycle belongs to the application, and the RFC 9605 Section 5.2 MLS layout depends on
// that:
//
//     KID = (context << (S + E)) + (sender_index << E) + (epoch % (1 << E))
//
// Only the low E bits of the MLS epoch travel, so for one sender the KID cycles through 1 << E
// values and every value recurs, each time naming key material the MLS exporter derived afresh. A
// send provider that treated a KID as spent for good could not rekey past the first wrap.
void testMlsEpochWrapRollsAndDecodes() {
	const uint8_t epochBits = 4;    // E
	const uint64_t senderIndex = 1; // one sender's slot
	const uint64_t kidBase = senderIndex << epochBits;
	const uint64_t epochs = (uint64_t(1) << epochBits) + 5; // past one full cycle

	// The premise, asserted rather than assumed: unless the walk runs past a full cycle no KID
	// recurs, and the test would pass without exercising the wrap at all.
	expect(epochs > (uint64_t(1) << epochBits),
	       "the walk must pass one full epoch cycle, or no KID recurs and this tests nothing");

	// ratchetStepBits 0: the MLS layout composes the whole KID itself and an epoch exports its own
	// key rather than deriving from the last one, so there is no ratchet field.
	auto sender = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
	    SFrameSendKey{masterKey(0x40), kidBase});
	auto receiver = std::make_shared<SFrameReceiveKeyProvider>(kSuite, /*ratchetStepBits=*/0,
	                                                           /*perSsrcDerivation=*/true);
	receiver->addKey(kidBase, SFrameReceiveKey{masterKey(0x40)});

	impl::SFrameEncoder encoder(sender, kSsrc);
	impl::SFrameDecoder decoder(receiver, kSsrc);

	for (uint64_t epoch = 1; epoch < epochs; ++epoch) {
		const uint64_t kid = kidBase + (epoch % (uint64_t(1) << epochBits));
		const binary key = masterKey(uint8_t(0x40 + epoch));

		// The whole point: from epoch 1 << E onward this KID has been sent under before.
		sender->rollKey(SFrameSendKey{key, kid});
		receiver->addKey(kid, SFrameReceiveKey{key});

		const auto plaintext = payload(uint8_t(0x80 + epoch));
		auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
		expect(decodes(decoder, frame, plaintext),
		       "a frame in MLS epoch " + to_string(epoch) + " did not decode");
		expect(impl::sframe::header::Decode(*frame).kid == kid,
		       "epoch " + to_string(epoch) + " sent under KID " +
		           to_string(impl::sframe::header::Decode(*frame).kid) + ", expected " +
		           to_string(kid));
	}

	// One sender's KIDs never leave its slot, so the receiver holds at most one cycle's worth
	// rather than one per epoch: RFC 9605 Section 5.2 has receivers remove an old epoch when a new
	// one with the same low-order E bits appears, which addKey() does by overwriting in place.
	expect(receiver->keyGenerations().size() == (size_t(1) << epochBits),
	       "expected " + to_string(size_t(1) << epochBits) + " generations after " +
	           to_string(epochs) + " epochs, got " + to_string(receiver->keyGenerations().size()));
}

// currentKey() must describe one point on the chain. Sampled field by field it can straddle a
// ratchet and pair the old key with a restarted counter, which replays every nonce already spent
// under it -- so this drives a live ratchet underneath repeated reads and checks the pairing.
void testCurrentKeyIsSampledAtomically() {
	auto sender = std::make_shared<SFrameSendKeyProvider>(kSuite, /*ratchetStepBits=*/4,
	                                                      /*ratchetPeriod=*/1,
	                                                      /*perSsrcDerivation=*/false,
	                                                      SFrameSendKey{masterKey(), kKid});
	auto encoder = impl::SFrameEncoder::ForTrack(sender, kSsrc);

	std::atomic<bool> stop{false};
	std::atomic<size_t> samples{0};
	std::map<uint64_t, binary> keyForKid;
	std::mutex seenMutex;

	ThreadFailure readerFailure;
	std::thread reader([&] {
		guarded(readerFailure, [&] {
			while (!stop.load()) {
				// Backs off rather than spinning: a tight loop on the same mutex starves the
				// encoding thread, and under a serialising tool like valgrind it never finishes.
				std::this_thread::sleep_for(std::chrono::microseconds(200));
				auto state = sender->currentKey();
				if (!state)
					continue;
				std::lock_guard<std::mutex> lock(seenMutex);
				auto it = keyForKid.find(state->kid);
				if (it == keyForKid.end())
					keyForKid.emplace(state->kid, state->baseKey);
				else
					expect(it->second == state->baseKey,
					       "currentKey() reported two different base keys for one KID, so a read "
					       "straddled a ratchet");
				++samples;
			}
		});
	});

	for (int i = 0; i < 120; ++i) {
		encoder->encodeFrame(make_message(payload(uint8_t(i)), Message::Binary), {});
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	stop.store(true);
	reader.join();
	readerFailure.rethrowIfAny();

	expect(samples.load() > 0, "the reader never sampled currentKey()");
	expect(keyForKid.size() > 1,
	       "the key never ratcheted during sampling, so the race was not exercised");
}

// An RTX retransmit or a reordered packet can deliver a frame under the previous generation after
// the roll. That key is still held, so the frame is not lost.
void testStragglerUnderPreviousGenerationStillDecodes() {
	auto sender = sendProvider();
	impl::SFrameEncoder encoder(sender, kSsrc);

	auto receiver = receiveProvider();
	receiver->addKey(kOtherKid, SFrameReceiveKey{masterKey(0x22)});
	impl::SFrameDecoder decoder(receiver, kSsrc);

	const auto inFlight = payload(0xD0);
	auto oldFrame = encoder.encodeFrame(make_message(binary(inFlight), Message::Binary), {});

	sender->rollKey(SFrameSendKey{masterKey(0x22), kOtherKid});
	const auto fresh = payload(0xD1);
	auto newFrame = encoder.encodeFrame(make_message(binary(fresh), Message::Binary), {});
	expect(decodes(decoder, newFrame, fresh), "the post-roll frame did not decode");

	expect(decodes(decoder, oldFrame, inFlight),
	       "an in-flight frame under the previous generation was lost");
}

// One live encoder ratcheting itself past 2^R, rather than a synthesised sender at each step.
// R is 3 here, the narrowest the unwrap supports, so the field wraps every eighth ratchet and a few
// seconds of traffic crosses it: the KID cycles, so the receiver cannot tell which turn of the
// field a step belongs to from the wire alone and has to read it against its chain head.
void testLiveRatchetWrapsAroundTheField() {
	const uint8_t bits = 3; // the narrowest field the unwrap can handle
	const uint64_t kid = kGeneration << bits;
	const uint64_t fieldSize = uint64_t(1) << bits;

	auto sender = std::make_shared<SFrameSendKeyProvider>(kSuite, bits, /*ratchetPeriod=*/1,
	                                                      /*perSsrcDerivation=*/true,
	                                                      SFrameSendKey{masterKey(), kid});
	impl::SFrameEncoder encoder(sender, kSsrc);

	auto provider = std::make_shared<SFrameReceiveKeyProvider>(kSuite, bits,
	                                                           /*perSsrcDerivation=*/true);
	provider->addKey(kid, SFrameReceiveKey{masterKey()});
	impl::SFrameDecoder decoder(provider, kSsrc);

	std::set<uint64_t> kids;
	for (int i = 0; i < 44; ++i) {
		const auto plaintext = payload(static_cast<uint8_t>(0xA0 + i));
		auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
		kids.insert(impl::sframe::header::Decode(*frame).kid);

		expect(decodes(decoder, frame, plaintext), "frame " + to_string(i) +
		                                               " did not decode at ratchet " +
		                                               to_string(encoder.ratchetCount()));

		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	// Crossing the field takes one ratchet per value it holds: at fieldSize ratchets the step has
	// returned to 0, which is the case unwrapRatchetStep() exists for. Anything fewer only walks up
	// to the last value without ever wrapping, so fieldSize itself is the threshold.
	expect(encoder.ratchetCount() >= fieldSize,
	       "only " + to_string(encoder.ratchetCount()) + " ratchets occurred, so the " +
	           to_string(fieldSize) + "-value field never wrapped and this tested nothing");
	expect(kids.size() == fieldSize, "expected the KID to cycle through " + to_string(fieldSize) +
	                                     " values across the wrap, saw " + to_string(kids.size()));
}

// A ratchet field too narrow to unwrap must be refused at construction rather than silently
// producing a stream the receiver reads as going backwards.
void testNarrowRatchetFieldsRefused() {
	// The receiver resolves a wire step against its chain head with half a window of tolerance,
	// settling an exact-half tie backwards. So the tolerance has to exceed the forward advance the
	// decoder accepts: at 1 bit there is none, and at 2 bits the tolerance is exactly
	// MaxForwardRatchet, so a legitimate +2 advance from a head at 2 mod 4 reads as 2 behind and
	// the chain never recovers. Both are refused at construction.
	for (uint8_t bits : {uint8_t(1), uint8_t(2)}) {
		bool threw = false;
		try {
			SFrameSendKeyProvider provider(kSuite, bits, /*ratchetPeriod=*/1,
			                               /*perSsrcDerivation=*/true,
			                               SFrameSendKey{masterKey(), uint64_t(1) << bits});
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		expect(threw, "a ratchet field of " + to_string(bits) + " bits was accepted");
	}

	// 0 means no ratcheting and 3 is the narrowest usable field, so both are accepted.
	SFrameReceiveKeyProvider none(kSuite, 0, true);
	SFrameReceiveKeyProvider narrow(kSuite, 3, true);
}

// keyAuthenticated() is the signal an application needs to retire an old generation after a
// roll. It must fire only for frames that authenticated -- receiveKey() cannot serve, since a
// forged frame naming any generation triggers that -- and only on a change, since the KID moves
// with every ratchet while the generation does not.
class AuthReportingProvider final : public SFrameReceiveKeyProvider {
public:
	AuthReportingProvider() : SFrameReceiveKeyProvider(kSuite, kRatchetStepBits, true) {}

	void keyAuthenticated(uint64_t keyGeneration) override {
		std::lock_guard<std::mutex> lock(mMutex);
		mReported.push_back(keyGeneration);
	}

	std::vector<uint64_t> reported() const {
		std::lock_guard<std::mutex> lock(mMutex);
		return mReported;
	}

private:
	mutable std::mutex mMutex;
	std::vector<uint64_t> mReported;
};

void testKeyAuthenticatedReportsGenerationChanges() {
	auto sender = sendProvider();
	impl::SFrameEncoder encoder(sender, kSsrc);

	auto receiver = std::make_shared<AuthReportingProvider>();
	receiver->addKey(kKid, SFrameReceiveKey{masterKey()});
	receiver->addKey(kOtherKid, SFrameReceiveKey{masterKey(0x22)});
	impl::SFrameDecoder decoder(receiver, kSsrc);

	// Several frames on one generation report it once, not once per frame.
	for (int i = 0; i < 4; ++i) {
		const auto plaintext = payload(static_cast<uint8_t>(0xB0 + i));
		auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
		expect(decodes(decoder, frame, plaintext), "frame " + to_string(i) + " did not decode");
	}
	expect(receiver->reported() == std::vector<uint64_t>{kGeneration},
	       "expected one report for the first generation, got " +
	           to_string(receiver->reported().size()));

	// A roll reports the new generation, which is what lets the old one be retired.
	sender->rollKey(SFrameSendKey{masterKey(0x22), kOtherKid});
	const auto fresh = payload(0xBF);
	auto rolled = encoder.encodeFrame(make_message(binary(fresh), Message::Binary), {});
	expect(decodes(decoder, rolled, fresh), "the post-roll frame did not decode");
	expect(receiver->reported() == std::vector<uint64_t>({kGeneration, kOtherGeneration}),
	       "the roll was not reported as a generation change");

	// A forged frame naming a generation the provider holds must not report it: it never
	// authenticates, so an attacker cannot drive an application into retiring a live key.
	{
		auto forged = encodeAtGeneration(kGeneration, 0, fresh, masterKey(0x99));
		expect(!decodes(decoder, forged, fresh), "a frame under the wrong key authenticated");
		expect(receiver->reported() == std::vector<uint64_t>({kGeneration, kOtherGeneration}),
		       "a forged frame was reported as authenticated");
	}
}

// Two generations live at once, each on its own ratchet chain at its own step. Not a configuration
// a real deployment is likely to produce, but the decoder keys its chains on the master key rather
// than on the stream, so it must read each generation's wire step against that generation's chain
// head. Read against the other's, a generation several steps behind its neighbour is taken as going
// backwards and refused for good. The two advance at different rates and every frame sits at a step
// new to its own chain, so nothing here can be served from the derived-key cache instead.
void testGenerationsHoldSeparateRatchetSteps() {
	auto provider = std::make_shared<RecordingKeyProvider>();
	provider->addKey(kKid, SFrameReceiveKey{masterKey()});
	provider->addKey(kOtherKid, SFrameReceiveKey{masterKey(0x22)});
	impl::SFrameDecoder decoder(provider, kSsrc);

	// One generation climbs two steps at a time, the steady-state maximum, and the other one step,
	// interleaved frame for frame. Both advances are legal on their own chain; neither is legal
	// read against the other's head once the two have drifted apart.
	uint64_t slowStep = 0;
	for (uint64_t fastStep = 0; fastStep <= 10; fastStep += 2) {
		const auto fast = payload(static_cast<uint8_t>(0x10 + fastStep));
		expect(decodes(decoder, encodeAtGeneration(kGeneration, fastStep, fast, masterKey()), fast),
		       "the faster generation did not decode at step " + to_string(fastStep));

		const auto slow = payload(static_cast<uint8_t>(0x20 + slowStep));
		expect(decodes(decoder,
		               encodeAtGeneration(kOtherGeneration, slowStep, slow, masterKey(0x22)), slow),
		       "the slower generation was refused at its step " + to_string(slowStep) +
		           " while the other was at step " + to_string(fastStep));
		++slowStep;
	}

	// The two ended up five steps apart, which is what makes reading one against the other fatal
	// rather than merely tolerated.
	expect(slowStep == 6, "the two generations did not end up at different steps");

	// And the slower one keeps advancing from its own head afterwards.
	for (; slowStep <= 7; ++slowStep) {
		const auto moving = payload(static_cast<uint8_t>(0x30 + slowStep));
		expect(decodes(decoder,
		               encodeAtGeneration(kOtherGeneration, slowStep, moving, masterKey(0x22)),
		               moving),
		       "the slower generation could not advance to step " + to_string(slowStep) +
		           " from its own chain head");
	}

	// Two chains from two generations of key material, so neither was serving the other's frames.
	expect(provider->requested() == std::set<uint64_t>({kGeneration, kOtherGeneration}),
	       "the provider was not asked for both key generations");
	expect(provider->keyGenerations() == std::vector<uint64_t>({kKid, kOtherKid}),
	       "a generation was dropped while both were in use");
}

// SFrameMaxRatchetStepBits is the documented upper bound on R, and the bound itself has to work:
// the generation is shifted up by R to compose the KID, so at the widest field the two halves take
// exactly half of the 64 bits each and any miscounting overflows one into the other. The narrow and
// over-wide fields are refused elsewhere; this pins the top of the usable range.
void testWidestRatchetFieldRoundTrips() {
	const uint8_t bits = SFrameMaxRatchetStepBits;

	auto receiver = std::make_shared<SFrameReceiveKeyProvider>(kSuite, bits,
	                                                           /*perSsrcDerivation=*/true);
	receiver->addKey(impl::sframe::MakeKid(kGeneration, 0, bits), SFrameReceiveKey{masterKey()});
	impl::SFrameDecoder decoder(receiver, kSsrc);

	// A sender on kGeneration sitting at `step`, with the widest ratchet field.
	auto encodeWideAtStep = [&](uint64_t step, const binary &plaintext) {
		auto key = impl::sframe::DeriveSSRCKey(kSsrc, masterKey(), kSuite);
		for (uint64_t i = 0; i < step; ++i)
			key = impl::sframe::RatchetKey(kSuite, key);

		auto sender = std::make_shared<SFrameSendKeyProvider>(
		    kSuite, bits, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/true,
		    SFrameSendKey{std::move(key), impl::sframe::MakeKid(kGeneration, step, bits)});
		impl::SFrameEncoder encoder(sender);
		return encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
	};

	// Step 0 and a couple of ratchets past it, so the step field carries something and the
	// generation sitting above it still comes back intact.
	for (uint64_t step = 0; step <= 2; ++step) {
		const auto plaintext = payload(static_cast<uint8_t>(0xF0 + step));
		auto frame = encodeWideAtStep(step, plaintext);
		const uint64_t kid = impl::sframe::header::Decode(*frame).kid;
		expect(impl::sframe::KeyGenerationFromKid(kid, bits) == kGeneration &&
		           impl::sframe::RatchetStepFromKid(kid, bits) == step,
		       "the KID at R=" + to_string(bits) + " did not carry generation " +
		           to_string(kGeneration) + " at step " + to_string(step));
		expect(decodes(decoder, frame, plaintext),
		       "step " + to_string(step) + " did not round-trip at R=" + to_string(bits));
	}

	// The widest generation and the widest step that fit alongside each other fill the KID exactly
	// and split back, so neither half has overflowed into the other.
	const uint64_t widest = (uint64_t(1) << bits) - 1;
	const uint64_t fullKid = impl::sframe::MakeKid(widest, widest, bits);
	expect(fullKid == 0xFFFFFFFFFFFFFFFFull,
	       "the widest generation and step at R=" + to_string(bits) +
	           " did not fill the KID, got " + to_string(fullKid));
	expect(impl::sframe::KeyGenerationFromKid(fullKid, bits) == widest &&
	           impl::sframe::RatchetStepFromKid(fullKid, bits) == widest,
	       "the widest KID at R=" + to_string(bits) + " did not split back into its two halves");

	// One generation past what fits is refused rather than wrapped onto another generation's KID,
	// which -- the KID being an HKDF label -- would have the two share a key.
	bool threw = false;
	try {
		impl::sframe::MakeKid(widest + 1, 0, bits);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	expect(threw, "a key generation too wide for R=" + to_string(bits) + " was composed anyway");
}

// rollKey() is documented to be safe while frames are being encrypted, and with per-SSRC derivation
// off one encoder serves every track -- so two senders and a roller all land on the one stored key
// and the one counter. What must hold is that every frame that came out is still readable and that
// no (KID, CTR) pair recurs: the nonce comes from the salt, which the KID seeds, and the counter,
// so a repeat is a nonce collision, and on the GCM suites that leaks the GHASH key.
void testConcurrentRollWhileEncrypting() {
	const uint64_t kidBase = 1000;
	const int rolls = 12;
	const int framesPerRoll = 4;
	// Twice what the rolls consume, so rolling is finished well before encoding is.
	const int framesPerThread = rolls * framesPerRoll * 2;

	// ratchetStepBits 0, so the KID moves only when the key rolls and every KID is a generation of
	// its own; derivation off, so the two senders share the provider's encoder and its counter.
	auto sender = std::make_shared<SFrameSendKeyProvider>(
	    kSuite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
	    SFrameSendKey{masterKey(0x10), kidBase});
	auto encoder = impl::SFrameEncoder::ForTrack(sender, kSsrc);

	// Every generation the roller will pass through, since which frames landed under which is
	// exactly what the interleaving decides.
	auto receiver = std::make_shared<SFrameReceiveKeyProvider>(kSuite, /*ratchetStepBits=*/0,
	                                                           /*perSsrcDerivation=*/false);
	for (int r = 0; r <= rolls; ++r)
		receiver->addKey(kidBase + uint64_t(r), SFrameReceiveKey{masterKey(uint8_t(0x10 + r))});

	struct Sent {
		message_ptr frame;
		binary plaintext;
		uint64_t kid;
		uint64_t ctr;
	};

	// A plaintext no other frame produces, so a frame decoding proves it decoded as itself rather
	// than as whichever frame happened to share its bytes.
	auto tagged = [](int thread, int index) {
		binary plaintext(64, std::byte(0x5A));
		plaintext[0] = std::byte(thread);
		plaintext[1] = std::byte(index & 0xFF);
		plaintext[2] = std::byte((index >> 8) & 0xFF);
		return plaintext;
	};

	std::atomic<int> produced{0};
	// One slot per thread, sized up front: each thread only ever touches its own.
	std::vector<std::vector<Sent>> sent(2);

	ThreadFailure encodeFailure;
	auto encodeLoop = [&](int thread) {
		guarded(encodeFailure, [&] {
			for (int i = 0; i < framesPerThread; ++i) {
				auto plaintext = tagged(thread, i);
				auto frame =
				    encoder->encodeFrame(make_message(binary(plaintext), Message::Binary), {});
				const auto header = impl::sframe::header::Decode(*frame);
				sent[thread].push_back(Sent{frame, std::move(plaintext), header.kid, header.ctr});
				++produced;
			}
		});
	};

	std::thread first(encodeLoop, 0);
	std::thread second(encodeLoop, 1);

	// Paced on the frame count rather than the clock: each roll waits until frames have been
	// produced under the key it is replacing and leaves frames still to come, so every roll lands
	// mid-flight without the test depending on how fast any thread runs.
	for (int r = 1; r <= rolls; ++r) {
		while (produced.load() < r * framesPerRoll * 2)
			std::this_thread::yield();

		sender->rollKey(SFrameSendKey{masterKey(uint8_t(0x10 + r)), kidBase + uint64_t(r)});
	}

	first.join();
	second.join();
	encodeFailure.rethrowIfAny();

	impl::SFrameDecoder decoder(receiver);
	std::set<std::pair<uint64_t, uint64_t>> nonceSpace;
	std::set<uint64_t> kids;
	for (const auto &perThread : sent) {
		for (const auto &entry : perThread) {
			expect(decodes(decoder, entry.frame, entry.plaintext),
			       "a frame encrypted while the key was rolling did not decode");
			expect(nonceSpace.emplace(entry.kid, entry.ctr).second,
			       "KID " + to_string(entry.kid) + " spent counter " + to_string(entry.ctr) +
			           " twice, so a nonce repeated");
			kids.insert(entry.kid);
		}
	}

	expect(sent[0].size() + sent[1].size() == size_t(2 * framesPerThread),
	       "not every frame the two senders produced was recorded");

	// Otherwise every roll landed before or after the encoding and nothing was concurrent.
	expect(kids.size() > 1, "the key never rolled while frames were being encrypted");
}

// Both providers refuse key material below the 128-bit floor, and must refuse it without
// disturbing what they already hold: a short key is a mistake in the application's key management,
// not a reason to lose the key in force or the generation already registered.
void testShortBaseKeysRefused() {
	auto refuses = [](const string &what, auto &&fn) {
		bool threw = false;
		try {
			fn();
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		expect(threw, what + " was accepted");
	};

	const size_t floor = impl::sframe::MinBaseKeySize();

	// rollKey(), at every length below the floor including empty.
	{
		auto sender = sendProvider();
		impl::SFrameEncoder encoder(sender, kSsrc);

		for (size_t len : {size_t(0), size_t(1), floor - 1})
			refuses("a rollKey() base key of " + to_string(len) + " bytes", [&] {
				sender->rollKey(SFrameSendKey{binary(len, std::byte(0x11)), kOtherKid});
			});

		// The key in force is untouched, so the refusals cost nothing: a receiver holding only the
		// original generation still reads what the encoder sends.
		auto receiver = receiveProvider();
		impl::SFrameDecoder decoder(receiver, kSsrc);
		const auto plaintext = payload(0xF5);
		auto frame = encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
		expect(decodes(decoder, frame, plaintext),
		       "a refused rollKey() disturbed the key already in force");
	}

	// addKey(), likewise, and the generation it named is not left half registered.
	{
		auto receiver = receiveProvider();
		for (size_t len : {size_t(0), size_t(1), floor - 1})
			refuses("an addKey() base key of " + to_string(len) + " bytes", [&] {
				receiver->addKey(kOtherKid, SFrameReceiveKey{binary(len, std::byte(0x11))});
			});

		expect(receiver->keyGenerations() == std::vector<uint64_t>{kKid},
		       "a refused addKey() registered the generation anyway");

		// And the floor itself is accepted, so this is not refusing everything.
		receiver->addKey(kOtherKid, SFrameReceiveKey{binary(floor, std::byte(0x11))});
		expect(receiver->keyGenerations() == std::vector<uint64_t>({kKid, kOtherKid}),
		       "a base key at the floor was not accepted");
	}

	// And the upper bound on R is enforced on the receive side too, not only where a send provider
	// happens to be constructed.
	refuses("a receive provider with a ratchet field of " +
	            to_string(SFrameMaxRatchetStepBits + 1) + " bits",
	        [] {
		        SFrameReceiveKeyProvider provider(kSuite, uint8_t(SFrameMaxRatchetStepBits + 1),
		                                          /*perSsrcDerivation=*/true);
	        });
}

// A joiner is handed the key as it stands part way along the chain -- which is what
// SFrameSendKeyProvider::currentKey() reports -- rather than the generation's root. The KID's step
// field is what says where on the chain it sits, so registering it has to pick the stream up from
// there. Taken as a root and ratcheted from zero instead, every frame fails its tag and the only
// symptom is a decode counter.
void testKeyRegisteredPartWayAlongTheChain() {
	const uint8_t bits = kRatchetStepBits;
	const uint64_t generation = 9;
	const uint64_t joinStep = 5;
	const binary master = masterKey();

	// Non-per-SSRC, so the chain is rooted at the base key and a key at step N is one value every
	// stream shares. Per-SSRC is the mode addKey() ignores a non-zero step in, checked below.
	auto keyAtStep = [&](uint64_t step) {
		binary key = master;
		for (uint64_t i = 0; i < step; ++i)
			key = impl::sframe::RatchetKey(kSuite, key);
		return key;
	};

	auto encodeAt = [&](uint64_t step, const binary &plaintext) {
		auto sender = std::make_shared<SFrameSendKeyProvider>(
		    kSuite, bits, /*ratchetPeriod=*/0, /*perSsrcDerivation=*/false,
		    SFrameSendKey{keyAtStep(step), impl::sframe::MakeKid(generation, step, bits)});
		impl::SFrameEncoder encoder(sender);
		return encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
	};

	auto receiver = std::make_shared<SFrameReceiveKeyProvider>(kSuite, bits,
	                                                           /*perSsrcDerivation=*/false);
	receiver->addKey(impl::sframe::MakeKid(generation, joinStep, bits),
	                 SFrameReceiveKey{keyAtStep(joinStep)});
	impl::SFrameDecoder decoder(receiver, kSsrc);

	auto atJoin = payload(0x51);
	expect(decodes(decoder, encodeAt(joinStep, atJoin), atJoin),
	       "a frame at the step the key was registered at did not decode");

	auto later = payload(0x52);
	expect(decodes(decoder, encodeAt(joinStep + 2, later), later),
	       "a frame two steps past the registered one did not decode, so the chain did not advance "
	       "from where the key was registered");

	// Counterfactual: the same key registered as a root must fail, or the assertions above would
	// hold however the step field were treated.
	auto asRoot = std::make_shared<SFrameReceiveKeyProvider>(kSuite, bits,
	                                                         /*perSsrcDerivation=*/false);
	asRoot->addKey(impl::sframe::MakeKid(generation, 0, bits),
	               SFrameReceiveKey{keyAtStep(joinStep)});
	impl::SFrameDecoder rootDecoder(asRoot, kSsrc);
	auto again = payload(0x53);
	expect(!decodes(rootDecoder, encodeAt(joinStep, again), again),
	       "a key registered at step 0 decoded a step-" + to_string(joinStep) +
	           " frame, so the step field is not being read");

	// Per-SSRC ratchets the derived key, so "the key at step N" is per SSRC and cannot be
	// registered per generation. The step is ignored there and the key taken as the root, so a
	// caller holding a KID off the wire -- carrying whatever step the sender had reached -- does
	// not have to mask it off first. That is the pattern the rest of this file relies on.
	auto perSsrc = std::make_shared<SFrameReceiveKeyProvider>(kSuite, bits,
	                                                          /*perSsrcDerivation=*/true);
	perSsrc->addKey(impl::sframe::MakeKid(kGeneration, 7, bits), SFrameReceiveKey{masterKey()});
	impl::SFrameDecoder perSsrcDecoder(perSsrc, kSsrc);

	auto fromRoot = payload(0x54);
	expect(decodes(perSsrcDecoder, encodeAtStep(0, fromRoot), fromRoot),
	       "a step-0 frame did not decode after registering the root key under a KID whose step "
	       "field was non-zero, so per-SSRC mode is not ignoring the step");
}

TestResult test_sframe_key_provider() {
	InitLogger(LogLevel::Warning);
	try {
		testCachedFrameStillProvesItsStep();
		testIdleEncoderIsWalkedForwardBySiblings();
		testDecoderFollowsRatchetSteps();
		testUnusableKeyIsNotARefusal();
		testJoinerCatchesUpOnFirstFrame();
		testLargeJumpRefusedAfterCatchUp();
		testForgedStepDoesNotPoisonChain();
		testDecoderAcceptsLateStep();
		testUnseenOlderStepRefused();
		testUnknownKidRefused();
		testProviderRekeyTakesEffect();
		testRatchetStepWrapsWithinField();
		testLiveRatchetWrapsAroundTheField();
		testNarrowRatchetFieldsRefused();
		testKeyAuthenticatedReportsGenerationChanges();
		testStragglerUnderPreviousGenerationStillDecodes();
		testForgedGenerationDoesNotResetChain();
		testRekeyAtNonZeroStepDecodesImmediately();
		testForwardRatchetIsBounded();
		testRatchetStepBitsMismatchFails();
		testDecoderWithoutSsrcSkipsPerSsrcDerivation();
		testInitialCatchUpBoundary();
		testCatchUpIsRateLimited();
		testProviderIsThreadSafe();
		testSendKeyRotationRekeys();
		testRekeyOverlapDecodesInFlightFrames();
		testRollAndRatchetResetTheCounter();
		testRollKeyAcceptsTheSameGeneration();
		testSharedKeyGivesEveryTrackOneEncoder();
		testRollKeyIgnoresTheKeyAlreadyInForce();
		testMlsEpochWrapRollsAndDecodes();
		testCurrentKeyIsSampledAtomically();
		testCurrentKeyReportsTheLiveCounter();
		testCurrentKeyResumesOnTheSameChain();
		testGenerationsHoldSeparateRatchetSteps();
		testWidestRatchetFieldRoundTrips();
		testConcurrentRollWhileEncrypting();
		testShortBaseKeysRefused();
		testKeyRegisteredPartWayAlongTheChain();
		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}

#endif // RTC_ENABLE_MEDIA
