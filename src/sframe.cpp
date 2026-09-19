/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "sframe.hpp"
#include "impl/internals.hpp"
#include "impl/sframeutility.hpp"
#include "impl/logcounter.hpp"
#include "impl/sframecrypto.hpp"

#include <algorithm>
#include <cstdlib>

namespace rtc {

/// Encoder
std::shared_ptr<const SFrameEncoder::KeyEpoch>
SFrameEncoder::makeEpoch(const SFrameKeyDetails& details, uint64_t kid) {
	// The sframe_key and sframe_salt depend on (base key, kid, cipher suite) but not on
	// the frame counter, so the whole HKDF chain runs here, once per key generation.
	auto keyLabel = impl::SFrameUtility::GetSFrameKeyLabel(kid, details.cipherSuiteId);
	auto saltLabel = impl::SFrameUtility::GetSFrameSaltLabel(kid, details.cipherSuiteId);

	impl::SFrameKeyInfo keyInfo = {details.baseKey, details.cipherSuiteId, keyLabel, saltLabel};
	impl::SFrameDerivedKeys derived = impl::SFrameCrypto::DeriveSFrameKeys(keyInfo);

	KeyEpoch epoch;
	epoch.details = details;
	epoch.kid = kid;
	epoch.encryptionKey = std::move(derived.contentEncryptionKey);
	epoch.salt = std::move(derived.sframeSalt);
	epoch.nonceSize = impl::SFrameCrypto::GetNonceSize(details.cipherSuiteId);
	return std::make_shared<const KeyEpoch>(std::move(epoch));
}

SFrameEncoder::SFrameEncoder(const SFrameConfig &config)
    : mLastRatchet(std::chrono::steady_clock::now()), mCtr(config.keyDetails.ctrStart),
      mRatchetPeriod(config.ratchetPeriod), mRatchetStepBits(config.ratchetStepBits) {
	// A short or empty base key still derives a usable key, but one an attacker can
	// derive too, so reject it here rather than encrypt with it.
	impl::SFrameUtility::ValidateBaseKey(config.keyDetails.baseKey);

	// RFC 9605 Section 4.4.1: a base key is for encryption or decryption, never both. The
	// role cannot be inferred, so an unmarked key is a configuration error.
	if (config.keyDetails.use != SFrameKeyUse::Encrypt)
		throw std::invalid_argument(
		    "SFrame base key must be marked SFrameKeyUse::Encrypt to send with it");

	if (config.ratchetStepBits > SFrameConfig::MaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameConfig::MaxRatchetStepBits));

	// Without a ratchet field the new key would be published under the KID it replaced,
	// leaving the receiver no way to tell that the key changed.
	if (config.ratchetPeriod > 0 && config.ratchetStepBits == 0)
		throw std::invalid_argument(
		    "SFrame ratcheting requires ratchetStepBits > 0: with no ratchet field the "
		    "ratcheted key would reuse the same KID");

	mKeyEpoch = makeEpoch(config.keyDetails,
	                      impl::SFrameUtility::MakeKid(config.keyGeneration, config.ratchetStep,
	                                             config.ratchetStepBits));
}

std::shared_ptr<Message> SFrameEncoder::encodeFrame(const std::shared_ptr<Message>& frame, const binary& metadata) {
	const binary &plaintext = *frame; // Message derives from binary

	// Take a key snapshot, ratcheting first if the period has elapsed.
	auto epoch = currentEpoch();

	// Track::outgoing() runs the chain on the caller's thread with no lock, so concurrent
	// send() calls reach here together and fetch_add hands each a distinct counter. Reusing
	// (key, CTR) reuses a nonce, which on the GCM suites leaks the GHASH key. A ratchet in
	// between is harmless: ratchetKey() never rewinds the counter.
	const uint64_t ctr = mCtr.fetch_add(1, std::memory_order_relaxed);

	binary ct = encodeSFrame(*epoch, ctr, plaintext, metadata);

	// Preserve frameInfo: the RTP packetizer only advances the timestamp when it is set.
	auto result = make_message(std::move(ct), Message::Binary);
	result->frameInfo = frame->frameInfo;
	return result;
}

std::shared_ptr<const SFrameEncoder::KeyEpoch> SFrameEncoder::currentEpoch() {
	std::lock_guard lock(mKeyMutex);
	if (shouldRatchetKey()) {
		ratchetKey();
	}
	return mKeyEpoch;
}

// Must be called with mKeyMutex held
bool SFrameEncoder::shouldRatchetKey() {
	// A ratchet period of zero means "never ratchet".
	if (mRatchetPeriod.count() == 0) {
		return false;
	}

	const auto now = std::chrono::steady_clock::now();
	return now - mLastRatchet >= mRatchetPeriod;
}

// Must be called with mKeyMutex held
void SFrameEncoder::ratchetKey() {
	const auto& details = mKeyEpoch->details;

	const binary newBaseKey =
	    impl::SFrameCrypto::RatchetKey(details.cipherSuiteId, details.baseKey);

	// Publish a new snapshot rather than mutating in place: a concurrent encodeFrame() is
	// still reading the old one. Re-deriving here keeps HKDF off the per-frame path.
	SFrameKeyDetails next = details;
	next.baseKey = newBaseKey;
	mKeyEpoch = makeEpoch(next, impl::SFrameUtility::NextRatchetKid(mKeyEpoch->kid, mRatchetStepBits));

	mLastRatchet = std::chrono::steady_clock::now();
}

binary SFrameEncoder::encodeSFrame(const KeyEpoch& epoch, uint64_t ctr, const binary& plaintext, const binary& metadata) {
	// Only the nonce changes per frame; the key and salt came from the epoch.
	binary nonce = impl::SFrameUtility::DeriveNonce(epoch.salt, ctr, epoch.nonceSize);

	impl::SFrameHeaderInfo headerInfo = {epoch.kid, ctr, 0};
	binary encodedHeader = impl::SFrameHeader::Encode(headerInfo);

	binary aad = impl::SFrameUtility::MakeAAD(encodedHeader, metadata);

	binary payload = impl::SFrameUtility::EncodePlaintext(
	    epoch.details.cipherSuiteId, epoch.encryptionKey, nonce, aad, plaintext);

	binary packet;
	packet.reserve(encodedHeader.size() + payload.size());
	packet.insert(packet.end(), encodedHeader.begin(), encodedHeader.end());
	packet.insert(packet.end(), payload.begin(),  payload.end());
	return packet;
}

/// Decoder
void SFrameDecoder::expireCachedKeys() {
	const auto now = std::chrono::steady_clock::now();
	mDerivedKeys.erase(std::remove_if(mDerivedKeys.begin(), mDerivedKeys.end(),
	                                  [&](const CachedKey &entry) {
		                                  return now - entry.lastUsed > KeyRetention;
	                                  }),
	                   mDerivedKeys.end());
}

void SFrameDecoder::rebindSsrc(uint32_t ssrc) {
	if (mSsrc == std::optional<uint32_t>(ssrc))
		return;

	// Everything derived hangs off the old SSRC. mLastCatchUp and mLastDecodeOk deliberately
	// survive, so rebinding cannot be used to re-arm the long ratchet walk.
	mSsrc = ssrc;
	mChains.clear();
	mDerivedKeys.clear();
}

// The wire carries only the low R bits of the ratchet step (RFC 9605 Section 5.1), so it
// is read as the value nearest the chain head: R defines a sliding window, and without
// this the step appears to jump backwards every 2^R ratchets and the stream never
// recovers. Returns the absolute step.
uint64_t SFrameDecoder::unwrapRatchetStep(uint64_t wireStep, uint8_t ratchetStepBits,
                                          uint64_t chainStep) const {
	if (ratchetStepBits == 0)
		return 0; // not ratcheted: the whole KID is the key generation

	if (ratchetStepBits >= 64)
		return wireStep;

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

// The chain for a master key, created on first use. Nothing is mutated until the Section 8
// derivation has succeeded, so a provider returning unusable material leaves the decoder as
// it was rather than half-rebuilt.
SFrameDecoder::Chain &SFrameDecoder::selectChain(const SFrameKeyDetails &keyDetails) {
	for (size_t i = 0; i < mChains.size(); ++i) {
		if (mChains[i].masterKey == keyDetails.baseKey &&
		    mChains[i].cipherSuiteId == keyDetails.cipherSuiteId) {
			if (i != 0) // keep most recently used first
				std::rotate(mChains.begin(), mChains.begin() + i, mChains.begin() + i + 1);
			return mChains.front();
		}
	}

	Chain chain;
	chain.masterKey = keyDetails.baseKey;
	chain.cipherSuiteId = keyDetails.cipherSuiteId;

	// Section 7 is optional, and the sender's choice decides: deriving when it did not
	// yields a different key and nothing authenticates.
	const bool perSsrc = mSsrc && mKeyProvider->usePerSSRCDerivation();
	chain.root = perSsrc ? impl::SFrameUtility::DeriveSSRCKey(*mSsrc, keyDetails.baseKey,
	                                                          keyDetails.cipherSuiteId)
	                     : keyDetails.baseKey;
	chain.key = chain.root;

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
                                                 const SFrameKeyDetails &keyDetails) {
	expireCachedKeys();

	// Before the step is unwrapped: the step is relative to this chain's head, and the KID
	// naming the chain is unauthenticated, so reading it against another chain's head would
	// place the frame at the wrong absolute step.
	Chain &chain = selectChain(keyDetails);

	const uint64_t step =
	    unwrapRatchetStep(impl::SFrameUtility::RatchetStepFromKid(kid, ratchetStepBits),
	                      ratchetStepBits, chain.step);

	// Matched on the absolute step as well as the KID, because the KID repeats every 2^R
	// ratchets. The base key is compared too: a provider may re-key a KID in place, and
	// cached material must not outlive the key it came from.
	for (size_t i = 0; i < mDerivedKeys.size(); ++i) {
		const auto &entry = mDerivedKeys[i];
		if (entry.key->kid == kid && entry.key->step == step &&
		    entry.key->cipherSuiteId == keyDetails.cipherSuiteId &&
		    entry.key->baseKey == keyDetails.baseKey) {
			if (i != 0) // keep most recently used first
				std::rotate(mDerivedKeys.begin(), mDerivedKeys.begin() + i,
				            mDerivedKeys.begin() + i + 1);

			KeyLookup lookup;
			lookup.derived = mDerivedKeys.front().key;
			lookup.fromCache = true;
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
		const bool stale = chain.established && now - mLastDecodeOk > CatchUpAfter;
		const bool mayCatchUp = (!chain.established || stale) &&
		                        now - mLastCatchUp >= CatchUpInterval;
		const uint64_t allowed = mayCatchUp ? MaxInitialRatchet : MaxForwardRatchet;

		if (advance > allowed)
			throw std::runtime_error("SFrame ratchet step is " + std::to_string(advance) +
			                         " ahead, beyond the accepted advance of " +
			                         std::to_string(allowed));

		if (advance > MaxForwardRatchet)
			mLastCatchUp = now;

		for (uint64_t i = 0; i < advance; ++i)
			lookup.chainKey = impl::SFrameUtility::RatchetKey(keyDetails.cipherSuiteId,
			                                            lookup.chainKey);
		lookup.advancesChain = true;
	}

	auto keyLabel = impl::SFrameUtility::GetSFrameKeyLabel(kid, keyDetails.cipherSuiteId);
	auto saltLabel = impl::SFrameUtility::GetSFrameSaltLabel(kid, keyDetails.cipherSuiteId);
	impl::SFrameKeyInfo keyInfo = {lookup.chainKey, keyDetails.cipherSuiteId, keyLabel,
	                               saltLabel};
	impl::SFrameDerivedKeys derived = impl::SFrameCrypto::DeriveSFrameKeys(keyInfo);

	DerivedKey entry;
	entry.kid = kid;
	entry.step = step;
	entry.cipherSuiteId = keyDetails.cipherSuiteId;
	entry.baseKey = keyDetails.baseKey;
	entry.encryptionKey = std::move(derived.contentEncryptionKey);
	entry.salt = std::move(derived.sframeSalt);
	lookup.derived = std::make_shared<const DerivedKey>(std::move(entry));
	return lookup;
}

// Called once the frame the lookup was made for has authenticated.
void SFrameDecoder::commitKey(KeyLookup &lookup) {
	const auto now = std::chrono::steady_clock::now();

	// The stream is live whichever way the key was found. A cache hit counts: a stream
	// sitting on one KID between ratchets is served entirely from the cache, and treating
	// that as silence would let a forged step re-arm the long walk once a second.
	mLastDecodeOk = now;

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

std::shared_ptr<Message> SFrameDecoder::decodeFrame(const std::shared_ptr<Message>& encodedFrame, const binary& metadata) {
	const binary &sFrame = *encodedFrame;

	binary ct = decodeSFrame(sFrame, metadata);

	return make_message(std::move(ct), Message::Binary);
}

binary SFrameDecoder::decodeSFrame(const binary& packet, const binary& metadata) {
	impl::SFrameHeaderInfo decodedHeaderInfo = impl::SFrameHeader::Decode(packet);

	auto header = binary(packet.begin(), packet.begin() + decodedHeaderInfo.length);
	auto payload = binary(packet.begin() + decodedHeaderInfo.length,packet.end());

	// Ask the provider every frame: the lookup is cheap next to the KDF, and it is what
	// lets a provider revoke or re-key a KID and have it take effect on the next frame.
	// The KID layout is the provider's, so ask it for R before splitting: the application
	// never sees a raw KID and never has to do this arithmetic itself.
	const uint8_t ratchetStepBits = mKeyProvider->ratchetStepBits();
	if (ratchetStepBits > SFrameConfig::MaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameConfig::MaxRatchetStepBits));

	const uint64_t keyGeneration =
	    impl::SFrameUtility::KeyGenerationFromKid(decodedHeaderInfo.kid, ratchetStepBits);
	auto keyDetails = mKeyProvider->getKeyDetails(keyGeneration);
	if (!keyDetails) {
		// An unknown KID is an ordinary condition on a hostile or stale stream. Refuse it
		// rather than deriving from whatever a provider might return as a placeholder.
		throw std::runtime_error("No SFrame key for key generation " +
		                         std::to_string(keyGeneration));
	}

	// RFC 9605 Section 4.4.1: a base key is for encryption or decryption, never both. A
	// provider handing back the key this endpoint sends under would have both directions
	// share a key stream.
	if (keyDetails->use != SFrameKeyUse::Decrypt)
		throw std::invalid_argument(
		    "SFrame base key must be marked SFrameKeyUse::Decrypt to receive with it");

	auto lookup = lookupKey(decodedHeaderInfo.kid, ratchetStepBits, *keyDetails);

	// Only the nonce depends on the frame counter.
	binary nonce = impl::SFrameUtility::DeriveNonce(
	    lookup.derived->salt, decodedHeaderInfo.ctr, lookup.derived->salt.size());

	binary aad = impl::SFrameUtility::MakeAAD(header, metadata);

	// Throws if the frame does not authenticate, which leaves the chain untouched.
	binary plaintext = impl::SFrameUtility::DecodeCiphertext(
	    lookup.derived->cipherSuiteId, lookup.derived->encryptionKey, nonce, aad, payload);

	commitKey(lookup);
	return plaintext;
}

} // namespace rtc
