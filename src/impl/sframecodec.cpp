/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframecodec.hpp"

#if RTC_ENABLE_MEDIA

#include "impl/internals.hpp"
#include "impl/logcounter.hpp"
#include "impl/sframecrypto.hpp"
#include "impl/sframeutility.hpp"

#include <algorithm>

namespace rtc::impl {

/// Encoder
shared_ptr<const SFrameEncoder::DerivedKey> SFrameEncoder::deriveKey(const binary &baseKey,
                                                                     uint64_t kid) const {
	// Depends on (base key, kid, cipher suite) but not on the frame counter, so the HKDF chain runs
	// here, once per KID, rather than per frame.
	const uint16_t suite = mKeyProvider->cipherSuiteId();
	auto derived = sframe::DeriveKeysForKid(baseKey, kid, suite);

	DerivedKey key;
	key.baseKey = baseKey;
	key.kid = kid;
	key.encryptionKey = std::move(derived.encryptionKey);
	key.salt = std::move(derived.salt);
	key.nonceSize = sframe_crypto::GetNonceSize(suite);
	return std::make_shared<const DerivedKey>(std::move(key));
}

SFrameEncoder::SFrameEncoder(shared_ptr<SFrameSendKeyProvider> keyProvider)
    : SFrameEncoder(std::move(keyProvider), nullopt) {}

SFrameEncoder::SFrameEncoder(shared_ptr<SFrameSendKeyProvider> keyProvider, SSRC ssrc)
    : SFrameEncoder(std::move(keyProvider), optional<SSRC>(ssrc)) {}

SFrameEncoder::SFrameEncoder(shared_ptr<SFrameSendKeyProvider> keyProvider, optional<SSRC> ssrc)
    : mOwnedKeyProvider(std::move(keyProvider)), mKeyProvider(mOwnedKeyProvider.get()),
      mSsrc(ssrc) {
	if (!mKeyProvider)
		throw std::invalid_argument("SFrame send key provider is null");

	// Establish the first key here, so bad key material is reported to whoever built the
	// encoder rather than from the media path on the first frame.
	refreshKey();
}

SFrameEncoder::SFrameEncoder(SFrameSendKeyProvider *keyProvider, optional<SSRC> ssrc)
    : mKeyProvider(keyProvider), mSsrc(ssrc) {
	refreshKey();
}

shared_ptr<SFrameEncoder>
SFrameEncoder::ForTrack(const shared_ptr<SFrameSendKeyProvider> &keyProvider, SSRC ssrc) {
	if (!keyProvider)
		throw std::invalid_argument("SFrame send key provider is null");

	// Sharing the provider's encoder is what keeps one key to one counter: a second encoder on
	// the same key would start its counter over and reuse every nonce, which on the GCM suites
	// leaks the GHASH key.
	if (!keyProvider->usePerSSRCDerivation())
		return keyProvider->mSharedEncoder;

	// Derivation on means the key differs per SSRC, so the counter does too. Memoised rather than
	// built per call: two encoders on one SSRC would derive the same key and both start at
	// ctrStart.
	std::lock_guard lock(keyProvider->mSsrcEncodersMutex);
	auto it = keyProvider->mSsrcEncoders.find(ssrc);
	if (it != keyProvider->mSsrcEncoders.end())
		return it->second;

	// Raw provider pointer, as mSharedEncoder takes it; see SFrameEncoder.
	auto encoder =
	    shared_ptr<SFrameEncoder>(new SFrameEncoder(keyProvider.get(), optional<SSRC>(ssrc)));
	keyProvider->mSsrcEncoders.emplace(ssrc, encoder);
	return encoder;
}

// Must be called with mKeyMutex held
void SFrameEncoder::adoptSendKey(const SFrameSendKey &sendKey) {
	// A short or empty base key still derives a usable key, but one an attacker can
	// derive too, so reject it here rather than encrypt with it.
	sframe::ValidateBaseKey(sendKey.baseKey);

	// Clears both this copy of the provider's key and, at the assignment below, the master key it
	// was derived from.
	sframe::SecretBinary baseKey = sendKey.baseKey;

	// draft-ietf-avtcore-rtp-sframe Section 7, applied to every generation rather than only
	// the first: a rekey that skipped it would put the two sides on different keys.
	if (mSsrc && mKeyProvider->usePerSSRCDerivation())
		baseKey = sframe::DeriveSSRCKey(*mSsrc, baseKey.bytes(), mKeyProvider->cipherSuiteId());

	// Used as issued: the key system already composed the generation and starting step into it.
	mDerivedKey = deriveKey(baseKey.bytes(), sendKey.kid);
	mRatchetCount = 0;

	// New key material, or a new KID, is a new nonce space: both are HKDF inputs to the content key
	// and the salt, so restarting the counter here cannot repeat a nonce under what it replaced.
	// SFrameSendKeyProvider::rollKey() is what guarantees one of the two actually changed -- it
	// ignores a re-supply of the key already in force, precisely so this reset cannot replay.
	mCtr = sendKey.ctrStart;
}

shared_ptr<Message> SFrameEncoder::encodeFrame(const shared_ptr<Message> &frame,
                                               const binary &metadata) {
	const binary &plaintext = *frame; // Message derives from binary

	// Key and counter together: a ratchet or roll between the two would pair a stale key with
	// a counter already spent under it.
	auto frameKey = claimFrameKey();

	binary ct = encodeSFrame(*frameKey.derived, frameKey.ctr, plaintext, metadata);

	// Preserve frameInfo: the RTP packetizer only advances the timestamp when it is set.
	auto result = make_message(std::move(ct), Message::Binary);
	result->frameInfo = frame->frameInfo;
	return result;
}

// Must be called with mKeyMutex held
void SFrameEncoder::refreshKeyLocked() {

	// One atomic load in the common case: the key is only copied when it has actually rolled.
	if (!mDerivedKey || mKeyProvider->keyVersion() != mKeyVersion) {
		// Key and version together, from one locked read. Taken apart, a roll landing between them
		// pairs the new key with the old version, and the next frame adopts that same key again --
		// resetting the counter and repeating every nonce already sent under it.
		auto stored = mKeyProvider->storedKey();

		// Clears this copy of the base key however the scope exits; the provider keeps its own.
		sframe::ScopedCleanse clearStored(stored.key.baseKey);

		adoptSendKey(stored.key);
		mKeyVersion = stored.version;
	} else {
		// The step is the m-line's, not this encoder's: any stream sending advances it for all of
		// them, so every stream currently sending carries the same KID. A stream returning from
		// idle is behind by however many steps accumulated while it was quiet and walks its own
		// key forward to meet them -- the keys are per SSRC even though the step is shared.
		ratchetTo(mKeyProvider->advanceRatchetStep());
	}
}

void SFrameEncoder::refreshKey() {
	std::lock_guard lock(mKeyMutex);
	refreshKeyLocked();
}

SFrameEncoder::FrameKey SFrameEncoder::claimFrameKey() {
	FrameKey claimed;
	{
		std::lock_guard lock(mKeyMutex);
		refreshKeyLocked();
		claimed = FrameKey{mDerivedKey, mCtr++};
	}

	// Outside our lock: the sweep takes the table then each sibling's key lock, so holding ours
	// would invert against a sibling sending concurrently.
	mKeyProvider->catchUpOtherEncoders(mSsrc);

	return claimed;
}

// Must be called with mKeyMutex held
void SFrameEncoder::ratchetTo(uint64_t target) {
	if (mRatchetCount >= target)
		return; // already there, and the counter must not be disturbed

	const uint16_t suite = mKeyProvider->cipherSuiteId();
	const uint8_t stepBits = mKeyProvider->ratchetStepBits();

	// Only the step this stops on needs a key and salt, so the derivation happens once below
	// rather than per step.
	sframe::SecretBinary baseKey = mDerivedKey->baseKey;
	uint64_t kid = mDerivedKey->kid;
	while (mRatchetCount < target) {
		baseKey = sframe_crypto::RatchetKey(suite, baseKey.bytes());
		kid = sframe::NextRatchetKid(kid, stepBits);
		++mRatchetCount;
	}

	// A new snapshot, so a concurrent encodeFrame() finishes against the key it claimed.
	mDerivedKey = deriveKey(baseKey.bytes(), kid);

	// The ratchet changed the KID, so this is a fresh nonce space too.
	mCtr = 0;
}

void SFrameEncoder::catchUpToStep(uint64_t target) {
	std::lock_guard lock(mKeyMutex);

	// Not adopted the current generation yet, so its step count is on the old chain. It adopts and
	// restarts at zero on its own next frame.
	if (mKeyProvider->keyVersion() != mKeyVersion)
		return;

	ratchetTo(target);
}

uint64_t SFrameEncoder::ratchetCount() const {
	std::lock_guard lock(mKeyMutex);
	return mRatchetCount;
}

uint64_t SFrameEncoder::currentCounter() const {
	std::lock_guard lock(mKeyMutex);
	return mCtr;
}

SFrameSendKey SFrameEncoder::currentState() const {
	std::lock_guard lock(mKeyMutex);
	SFrameSendKey state;
	if (mDerivedKey) {
		state.baseKey = mDerivedKey->baseKey.bytes();
		state.kid = mDerivedKey->kid;
	}
	state.ctrStart = mCtr;
	return state;
}

binary SFrameEncoder::encodeSFrame(const DerivedKey &key, uint64_t ctr, const binary &plaintext,
                                   const binary &metadata) {
	// Only the nonce changes per frame; the key and salt were derived once.
	binary nonce = sframe::DeriveNonce(key.salt.bytes(), ctr, key.nonceSize);

	// The counter travels in the clear, so a freed nonce is the salt with known bits flipped.
	sframe::ScopedCleanse clearNonce(nonce);

	SFrameHeaderInfo headerInfo = {key.kid, ctr, 0};
	binary encodedHeader = sframe::header::Encode(headerInfo);

	binary aad = sframe::MakeAAD(encodedHeader, metadata);

	binary payload = sframe::EncodePlaintext(mKeyProvider->cipherSuiteId(),
	                                         key.encryptionKey.bytes(), nonce, aad, plaintext);

	binary packet;
	packet.reserve(encodedHeader.size() + payload.size());
	packet.insert(packet.end(), encodedHeader.begin(), encodedHeader.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

/// Decoder
void SFrameDecoder::expireCachedKeys() {
	const auto now = std::chrono::steady_clock::now();
	mDerivedKeys.erase(
	    std::remove_if(mDerivedKeys.begin(), mDerivedKeys.end(),
	                   [&](const CachedKey &entry) { return now - entry.lastUsed > KeyRetention; }),
	    mDerivedKeys.end());
}

// The wire carries only the low R bits of the ratchet step (RFC 9605 Section 5.1), so it
// is read as the value nearest the chain head: R defines a sliding window, and without
// this the step appears to jump backwards every 2^R ratchets and the stream never
// recovers. Returns the absolute step.
uint64_t SFrameDecoder::unwrapRatchetStep(uint64_t wireStep, uint8_t ratchetStepBits,
                                          uint64_t chainStep) const {
	if (ratchetStepBits == 0)
		return 0; // not ratcheted: the whole KID is the key generation

	const uint64_t window = uint64_t(1) << ratchetStepBits;
	const uint64_t mask = window - 1;
	const uint64_t candidate = (chainStep & ~mask) | (wireStep & mask);

	// Half a window each way, as for RTP sequence numbers.
	if (candidate + window / 2 < chainStep)
		return candidate + window; // wrapped forward past the top of the window
	if (candidate > chainStep + window / 2 && candidate >= window)
		return candidate - window; // genuinely behind, seen across the wrap
	return candidate;
}

// The chain for a master key, created on first use. Nothing is mutated until the Section 7
// derivation has succeeded, so a provider returning unusable material leaves the decoder as
// it was rather than half-rebuilt.
SFrameDecoder::Chain &SFrameDecoder::selectChain(const SFrameReceiveKey &receiveKey,
                                                 uint64_t registeredStep) {
	for (size_t i = 0; i < mChains.size(); ++i) {
		if (sframe::ConstantTimeEquals(mChains[i].masterKey.bytes(), receiveKey.baseKey)) {
			if (i != 0) // keep most recently used first
				std::rotate(mChains.begin(), mChains.begin() + i, mChains.begin() + i + 1);
			return mChains.front();
		}
	}

	Chain chain;
	chain.masterKey = receiveKey.baseKey;

	// Checked before the chain exists, so the catch-up walk cannot run thousands of HKDF rounds on
	// material that is going to be refused anyway. The per-SSRC branch below validates on its own;
	// a receiveKey() override is what can hand this path a key addKey() never saw.
	sframe::ValidateBaseKey(receiveKey.baseKey);

	// Section 7 is optional, and the sender's choice decides: deriving when it did not
	// yields a different key and nothing authenticates.
	const bool perSsrc = mSsrc && mKeyProvider->usePerSSRCDerivation();
	chain.root =
	    perSsrc ? sframe::DeriveSSRCKey(*mSsrc, receiveKey.baseKey, mKeyProvider->cipherSuiteId())
	            : receiveKey.baseKey;
	chain.key = chain.root;

	// Where the registered key sits on the chain, so a key already advanced along it is taken as it
	// is rather than as a root that then gets ratcheted from zero. Always 0 with per-SSRC
	// derivation, where addKey() ignores the step with a warning rather than storing it.
	chain.step = registeredStep;

	if (mChains.size() >= MaxChains) {
		// Drop a chain nothing ever decoded on before one that is carrying media: only the
		// former can have been created by a peer naming a key generation it cannot use.
		auto victim = std::find_if(mChains.rbegin(), mChains.rend(),
		                           [](const Chain &c) { return !c.established; });
		mChains.erase(victim != mChains.rend() ? std::prev(victim.base())
		                                       : std::prev(mChains.end()));
	}

	mChains.insert(mChains.begin(), std::move(chain));
	return mChains.front();
}

// draft-ietf-avtcore-rtp-sframe Section 8: the per-SSRC derivation runs once, and every
// ratchet advances that key rather than re-deriving from a ratcheted base key. The sender
// does the same, so the two only agree past step 0 by walking the same chain.
//
// The chain head is not advanced here: the caller commits only once the frame
// authenticates, so an unauthenticated frame cannot move it. It can spend the catch-up
// rate limit, which is what bounds the work a forged step can force.
SFrameDecoder::KeyLookup SFrameDecoder::lookupKey(uint64_t kid, uint8_t ratchetStepBits,
                                                  const SFrameReceiveKey &receiveKey) {
	expireCachedKeys();

	// Before the step is unwrapped: the step is relative to this chain's head, and the KID
	// naming the chain is unauthenticated, so reading it against another chain's head would
	// place the frame at the wrong absolute step.
	Chain &chain = selectChain(receiveKey, mKeyProvider->chainStepFor(kid));

	// The wire carries only the low R bits, so unwrapping needs a reference near the sender's real
	// position. A chain's own head is that, except when the chain is new -- rebuilt after its
	// decoder was evicted, say -- where a head of 0 makes every wrap branch unreachable and the
	// step resolves to `wireStep mod 2^R`, off by a multiple of the field size once the sender has
	// wrapped. The furthest step proven on this m-line is the reference to use then: the sender
	// moves every stream's step together, so it is near this stream's too.
	const uint64_t proven = mLimiter->provenStep(receiveKey.baseKey);

	const uint64_t step = unwrapRatchetStep(sframe::RatchetStepFromKid(kid, ratchetStepBits),
	                                        ratchetStepBits, std::max(chain.step, proven));

	// Matched on the absolute step as well as the KID, because the KID repeats every 2^R
	// ratchets. The base key is compared too: a provider may re-key a KID in place, and
	// cached material must not outlive the key it came from.
	for (size_t i = 0; i < mDerivedKeys.size(); ++i) {
		const auto &entry = mDerivedKeys[i];
		if (entry.key->kid == kid && entry.key->step == step &&
		    sframe::ConstantTimeEquals(entry.key->baseKey.bytes(), receiveKey.baseKey)) {
			if (i != 0) // keep most recently used first
				std::rotate(mDerivedKeys.begin(), mDerivedKeys.begin() + i,
				            mDerivedKeys.begin() + i + 1);

			KeyLookup lookup;
			lookup.derived = mDerivedKeys.front().key;
			lookup.fromCache = true;
			// The absolute step this cache entry stands for. Left unset, commitKey() would record
			// the m-line as proven only to step 0, and a sibling returning from idle would be
			// refused the walk it is owed. The update path there hides it behind a std::max; the
			// insert path, reached whenever this master key's entry has been evicted, does not.
			lookup.step = step;
			return lookup;
		}
	}

	if (step < chain.step) {
		// Older than the chain head and uncached. Walking back means rebuilding from the
		// root, a chain per step, which a forged step could force repeatedly -- so late
		// frames are served from the cache or not at all.
		throw std::runtime_error("SFrame ratchet step " + std::to_string(step) +
		                         " is behind the current chain and no longer cached");
	}

	KeyLookup lookup;
	lookup.step = step;
	lookup.chainKey = chain.key;

	if (step > chain.step) {
		const uint64_t advance = step - chain.step;
		const auto now = std::chrono::steady_clock::now();

		// A long walk is allowed when the chain has not been established -- a joiner's
		// first frame can be far into a running session -- and again once the stream has
		// been silent long enough to have ratcheted out of the steady-state window, which
		// is what lets a paused or disconnected receiver recover. Rate limiting it, rather
		// than counting attempts, is what keeps a forged step from either exhausting a
		// genuine joiner's allowance or forcing a walk per frame.
		const bool stale = chain.established && now - mLimiter->lastDecodeOk > CatchUpAfter;
		const bool mayCatchUp =
		    (!chain.established || stale) && now - mLimiter->lastCatchUp >= CatchUpInterval;
		const uint64_t allowed = mayCatchUp ? MaxInitialRatchet : MaxForwardRatchet;

		// A step a sibling has already authenticated at is expected, not speculative: the sender
		// moves every stream together, so one coming back from idle is behind by exactly that much.
		// MaxForwardRatchet of slack past the mark, because the returning stream is often the one
		// that opens the period and so arrives a step ahead of it.
		const bool expected = step <= proven + MaxForwardRatchet;

		// Expected earns the joiner's allowance, not an unlimited one: a fresh SSRC resolves
		// against the mark from a chain at zero, and a frame that fails its tag commits nothing, so
		// the next one repeats the walk. Not rate limited, unlike the joiner's -- streams do arrive
		// together, and each needs its own walk. Untestable: reaching it needs a proven step above
		// the ceiling, which the ceiling itself prevents.
		const uint64_t ceiling = expected ? MaxInitialRatchet : allowed;
		if (advance > ceiling)
			throw std::runtime_error("SFrame ratchet step is " + std::to_string(advance) +
			                         " ahead, beyond the accepted advance of " +
			                         std::to_string(ceiling));

		if (advance > MaxForwardRatchet)
			mLimiter->lastCatchUp = now;

		for (uint64_t i = 0; i < advance; ++i)
			lookup.chainKey =
			    sframe::RatchetKey(mKeyProvider->cipherSuiteId(), lookup.chainKey.bytes());
		lookup.advancesChain = true;
	}

	const uint16_t suite = mKeyProvider->cipherSuiteId();
	auto derived = sframe::DeriveKeysForKid(lookup.chainKey.bytes(), kid, suite);

	DerivedKey entry;
	entry.kid = kid;
	entry.step = step;
	entry.baseKey = receiveKey.baseKey;
	entry.encryptionKey = std::move(derived.encryptionKey);
	entry.salt = std::move(derived.salt);
	lookup.derived = std::make_shared<const DerivedKey>(std::move(entry));
	return lookup;
}

// Called once the frame the lookup was made for has authenticated.
void SFrameDecoder::commitKey(KeyLookup &lookup) {
	const auto now = std::chrono::steady_clock::now();

	// The frame authenticated, so this generation is the peer's rather than a KID it invented.
	// Reported only on a change: the KID moves with every ratchet, but the generation is what a
	// provider keys its material on.
	const uint64_t generation =
	    sframe::KeyGenerationFromKid(lookup.derived->kid, mKeyProvider->ratchetStepBits());
	if (mLastAuthenticatedGeneration != generation) {
		mLastAuthenticatedGeneration = generation;
		mKeyProvider->keyAuthenticated(generation);
	}

	// The stream is live whichever way the key was found. A cache hit counts: a stream
	// sitting on one KID between ratchets is served entirely from the cache, and treating
	// that as silence would let a forged step re-arm the long walk once a second.
	mLimiter->lastDecodeOk = now;

	// This step is now proven for the whole m-line, so a sibling stream that was idle may walk to
	// it without being charged as speculative.
	mLimiter->recordProven(lookup.derived->baseKey.bytes(), lookup.step);

	if (lookup.fromCache) {
		if (!mDerivedKeys.empty())
			mDerivedKeys.front().lastUsed = now;
		return;
	}

	// selectChain() left the chain this lookup was made against at the front, and nothing
	// between there and here touches mChains.
	Chain &chain = mChains.front();
	if (lookup.advancesChain) {
		chain.key = std::move(lookup.chainKey);
		chain.step = lookup.step;
	}
	chain.established = true;

	if (mDerivedKeys.size() >= MaxCachedKeys)
		mDerivedKeys.pop_back();
	mDerivedKeys.insert(mDerivedKeys.begin(), CachedKey{lookup.derived, now});
}

shared_ptr<Message> SFrameDecoder::decodeFrame(const shared_ptr<Message> &encodedFrame,
                                               const binary &metadata) {
	const binary &sFrame = *encodedFrame;

	binary ct = decodeSFrame(sFrame, metadata);

	return make_message(std::move(ct), Message::Binary);
}

binary SFrameDecoder::decodeSFrame(const binary &packet, const binary &metadata) {
	SFrameHeaderInfo decodedHeaderInfo = sframe::header::Decode(packet);

	auto header = binary(packet.begin(), packet.begin() + decodedHeaderInfo.length);
	auto payload = binary(packet.begin() + decodedHeaderInfo.length, packet.end());

	// Ask the provider every frame: the lookup is cheap next to the KDF, and it is what
	// lets a provider revoke or re-key a KID and have it take effect on the next frame.
	// The KID layout is the provider's, so ask it for R before splitting: the application passes
	// and receives whole KIDs and never has to do this arithmetic itself.
	// Validated when the provider was constructed, so it cannot have drifted since.
	const uint8_t ratchetStepBits = mKeyProvider->ratchetStepBits();

	auto receiveKey = mKeyProvider->receiveKey(decodedHeaderInfo.kid);
	if (!receiveKey) {
		// An unknown KID is an ordinary condition on a hostile or stale stream. Refuse it
		// rather than deriving from whatever a provider might return as a placeholder.
		throw std::runtime_error("No SFrame key for KID " + std::to_string(decodedHeaderInfo.kid));
	}

	// The provider hands back the master key by value, so this copy is the library's and is cleared
	// however the scope exits. Without it a receiver leaves one copy per frame in freed heap.
	sframe::ScopedCleanse clearReceiveKey(receiveKey->baseKey);

	auto lookup = lookupKey(decodedHeaderInfo.kid, ratchetStepBits, *receiveKey);

	// Only the nonce depends on the frame counter.
	binary nonce = sframe::DeriveNonce(lookup.derived->salt.bytes(), decodedHeaderInfo.ctr,
	                                   lookup.derived->salt.size());

	// The counter travels in the clear, so a freed nonce is the salt with known bits flipped.
	sframe::ScopedCleanse clearNonce(nonce);

	binary aad = sframe::MakeAAD(header, metadata);

	// Throws if the frame does not authenticate, which leaves the chain untouched.
	binary plaintext = sframe::DecodeCiphertext(
	    mKeyProvider->cipherSuiteId(), lookup.derived->encryptionKey.bytes(), nonce, aad, payload);

	commitKey(lookup);
	return plaintext;
}

SFrameDecoderSet::SFrameDecoderSet(shared_ptr<SFrameReceiveKeyProvider> keyProvider)
    : mKeyProvider(std::move(keyProvider)),
      mLimiter(std::make_shared<SFrameDecoder::CatchUpLimiter>()) {
	if (!mKeyProvider)
		throw std::invalid_argument("SFrame key provider is null");
}

SFrameDecoder &SFrameDecoderSet::forSsrc(SSRC ssrc) {
	// Without the Section 7 derivation the SSRC is no part of the key schedule: every stream on the
	// m-line shares one master key, one sender-side ratchet and therefore one chain. Splitting per
	// SSRC would have each decoder maintain a byte-identical chain off its own traffic, so a stream
	// that went quiet would fall behind the others and then be refused the catch-up needed to
	// rejoin -- the busy streams keep the shared allowance looking live. One decoder for the m-line
	// removes the divergence rather than compensating for it, and saves the duplicated HKDF work
	// and derived-key caches with it.
	if (!mKeyProvider->usePerSSRCDerivation()) {
		if (mDecoders.empty()) {
			Entry shared;
			shared.decoder = std::make_unique<SFrameDecoder>(mKeyProvider, mLimiter);
			mDecoders.emplace(kSharedDecoderKey, std::move(shared));
		}

		auto &entry = mDecoders.begin()->second;
		entry.lastUse = ++mUses;
		return *entry.decoder;
	}

	auto it = mDecoders.find(ssrc);
	if (it == mDecoders.end()) {
		// Evict before inserting, and never the stream being asked for.
		while (mDecoders.size() >= MaxDecoders) {
			auto idlest = mDecoders.begin();
			for (auto i = mDecoders.begin(); i != mDecoders.end(); ++i)
				if (i->second.lastUse < idlest->second.lastUse)
					idlest = i;
			mDecoders.erase(idlest);
		}

		Entry entry;
		entry.decoder = std::make_unique<SFrameDecoder>(mKeyProvider, ssrc, mLimiter);
		it = mDecoders.emplace(ssrc, std::move(entry)).first;
	}

	it->second.lastUse = ++mUses;
	return *it->second.decoder;
}

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA
