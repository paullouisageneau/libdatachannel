/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframe.hpp"

#include "impl/internals.hpp"
#include "impl/logcounter.hpp"
#include "impl/sframecodec.hpp"
#include "impl/sframecrypto.hpp"
#include "impl/sframeutility.hpp"

namespace rtc {

SFrameKeyProvider::SFrameKeyProvider(uint16_t cipherSuiteId, uint8_t ratchetStepBits,
                                     bool perSsrcDerivation)
    : mCipherSuiteId(cipherSuiteId), mRatchetStepBits(ratchetStepBits),
      mPerSsrcDerivation(perSsrcDerivation) {
	if (mRatchetStepBits > SFrameMaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameMaxRatchetStepBits));

	// The receiver reads a wire step against its chain head with half a window of tolerance either
	// way, as for RTP sequence numbers, and resolves an exact-half tie backwards. So the tolerance
	// must exceed the forward advance the decoder accepts, or a legitimate ratchet reads as going
	// backwards: at 2 bits the window is 4 and the tolerance 2, which is exactly
	// SFrameDecoder::MaxForwardRatchet, and a +2 advance from a head at 2 mod 4 is taken as 2
	// behind. The frame then decrypts against a stale cached key, fails its tag, and the head never
	// advances again. Three bits is the narrowest that works.
	if (mRatchetStepBits > 0 && mRatchetStepBits < 3)
		throw std::invalid_argument(
		    "SFrame ratchetStepBits of " + std::to_string(mRatchetStepBits) +
		    " is too narrow to unwrap: use 0 for no ratcheting, or at least 3");

	// Rejected here rather than at the first frame: an unregistered suite has no key size,
	// nonce size or tag length to work from. 0 is what an uninitialised field carries.
	impl::sframe::ValidateCipherSuite(mCipherSuiteId);
}

SFrameSendKeyProvider::SFrameSendKeyProvider(uint16_t cipherSuiteId, uint8_t ratchetStepBits,
                                             uint16_t ratchetPeriod, bool perSsrcDerivation,
                                             SFrameSendKey key)
    : SFrameKeyProvider(cipherSuiteId, ratchetStepBits, perSsrcDerivation),
      mRatchetPeriod(ratchetPeriod), mLastRatchet(std::chrono::steady_clock::now()),
      mKey(std::move(key)) {
	impl::sframe::ValidateBaseKey(mKey.baseKey);

	// Without a ratchet field the ratcheted key would be published under the KID it replaced,
	// leaving the receiver no way to tell that the key changed.
	if (mRatchetPeriod > 0 && ratchetStepBits == 0)
		throw std::invalid_argument(
		    "SFrame ratcheting requires ratchetStepBits > 0: with no ratchet field the "
		    "ratcheted key would reuse the same KID");

	// Every track shares this one, so it is built here rather than by whichever sends first. It
	// takes the provider raw; see SFrameEncoder.
	if (!perSsrcDerivation)
		mSharedEncoder.reset(new impl::SFrameEncoder(this, nullopt));
}

// Out of line because impl::SFrameEncoder is incomplete in the header.
SFrameSendKeyProvider::~SFrameSendKeyProvider() {
	// The encoders clear what they derived from this; the stored key itself is cleared here.
	impl::sframe::Cleanse(mKey.baseKey);
}

void SFrameSendKeyProvider::rollKey(SFrameSendKey key) {
	impl::sframe::ValidateBaseKey(key.baseKey);

	// Cleared however this returns. On a roll the buffer is moved into mKey and this is a no-op on
	// an emptied vector; on the idempotent early return below it is the only thing that clears it.
	impl::sframe::ScopedCleanse clearKey(key.baseKey);

	std::lock_guard lock(mKeyMutex);

	// Re-supplying the key in force is a no-op rather than a roll: adopting it would restart the
	// counter at ctrStart and repeat every nonce already emitted. See rollKey().
	if (key.kid == mKey.kid && impl::sframe::ConstantTimeEquals(key.baseKey, mKey.baseKey))
		return;

	// A KID already sent under is otherwise accepted so long as the material differs, which the
	// RFC 9605 Section 5.2 MLS layout needs. See rollKey().
	//
	// The key being replaced is revoked at this point, so its bytes are cleared rather than left
	// behind in freed heap. Encoders derive their own copies and clear those as they roll off them.
	impl::sframe::Cleanse(mKey.baseKey);
	mKey = std::move(key);

	// New material starts a new chain, so the shared step and the sweep mark restart with it and
	// the period runs from here.
	mRatchetStep = 0;
	mSweptStep = 0;
	mLastRatchet = std::chrono::steady_clock::now();

	// Released after the key is stored, so an encoder that sees the new version reads the new
	// key rather than the one it replaced.
	mKeyVersion.fetch_add(1, std::memory_order_release);
}

void SFrameSendKeyProvider::catchUpOtherEncoders(optional<SSRC> sending) {
	// One encoder serves every track with derivation off, so nobody can fall behind.
	if (!usePerSSRCDerivation())
		return;

	uint64_t target;
	{
		std::lock_guard lock(mKeyMutex);
		if (mRatchetStep == mSweptStep)
			return; // once per step, not once per frame

		mSweptStep = mRatchetStep;
		target = mRatchetStep;
	}

	// Key lock released above: ForTrack() holds the table while reaching back for the key, so
	// holding it here would invert against it.
	std::lock_guard lock(mSsrcEncodersMutex);
	for (auto &entry : mSsrcEncoders)
		if (!sending || entry.first != *sending)
			entry.second->catchUpToStep(target);
}

uint64_t SFrameSendKeyProvider::advanceRatchetStep() {
	if (mRatchetPeriod == 0)
		return 0; // never ratchets

	const auto now = std::chrono::steady_clock::now();

	std::lock_guard lock(mKeyMutex);
	// One advance per elapsed period however many streams ask, since the timestamp is shared: a
	// second stream sending moments later sees it already reset. After a silence the step moves by
	// one on the next frame, not by the number of periods that passed.
	if (now - mLastRatchet >= std::chrono::seconds(mRatchetPeriod)) {
		++mRatchetStep;
		mLastRatchet = now;
	}

	return mRatchetStep;
}

SFrameSendKeyProvider::StoredKey SFrameSendKeyProvider::storedKey() const {
	// rollKey() stores the key and bumps the version under this same lock, so the pair cannot
	// straddle a roll.
	std::lock_guard lock(mKeyMutex);
	return {mKey, mKeyVersion.load(std::memory_order_relaxed)};
}

optional<SFrameSendKey> SFrameSendKeyProvider::currentKey() const {
	// Per-SSRC derivation gives every track its own key and counter, so there is nothing single
	// to report; without it one encoder serves the session and its counter is the session's.
	if (!mSharedEncoder)
		return nullopt;

	// The ratcheted key, not the one rollKey() stored, and all three fields from one locked
	// sample: read separately they can straddle a ratchet and describe two different points.
	return mSharedEncoder->currentState();
}

SFrameReceiveKeyProvider::~SFrameReceiveKeyProvider() {
	// Nothing can be consulting the map by now, so this clears whatever generations are left rather
	// than waiting for a removeKey() the application may never make.
	for (auto &entry : mKeys)
		impl::sframe::Cleanse(entry.second.key.baseKey);
}

void SFrameReceiveKeyProvider::addKey(uint64_t kid, SFrameReceiveKey key) {
	impl::sframe::ValidateBaseKey(key.baseKey);

	const uint8_t bits = ratchetStepBits();
	const uint64_t keyGeneration = impl::sframe::KeyGenerationFromKid(kid, bits);
	const uint64_t chainStep = impl::sframe::RatchetStepFromKid(kid, bits);

	// Per-SSRC derivation ratchets the derived key rather than the base key, so "the key at step N"
	// is per SSRC and cannot be registered here, which is per generation. The step is ignored in
	// that mode rather than rejected: a caller naturally has a KID off the wire, carrying whatever
	// step the sender had reached, and demanding it be masked off first would be a trap of its own.
	// Warned about because a caller who really did advance the key themselves gets nothing back
	// otherwise. currentKey() reports nullopt in this mode, so it cannot be the source of one.
	const bool perSsrcDerivation = usePerSSRCDerivation();
	if (chainStep != 0 && perSsrcDerivation) {
		PLOG_WARNING << "SFrame: ignoring ratchet step " << chainStep
		             << " on a registered key, since per-SSRC derivation ratchets the derived key "
		                "rather than the base key; it is taken as the generation's root";
	}

	std::unique_lock lock(mKeysMutex);

	// Replacing a generation revokes the old material at once -- the decoder consults this every
	// frame -- so clear it rather than leave it in freed heap.
	auto &slot = mKeys[keyGeneration];
	impl::sframe::Cleanse(slot.key.baseKey);
	slot.key = std::move(key);
	slot.chainStep = perSsrcDerivation ? 0 : chainStep;
}

bool SFrameReceiveKeyProvider::removeKey(uint64_t kid) {
	const uint64_t keyGeneration = impl::sframe::KeyGenerationFromKid(kid, ratchetStepBits());

	std::unique_lock lock(mKeysMutex);
	auto it = mKeys.find(keyGeneration);
	if (it == mKeys.end())
		return false;

	// RFC 9605 Section 5.1 asks for a retired key to be deleted; clear the bytes as well as the
	// entry, so the material does not survive in freed heap.
	impl::sframe::Cleanse(it->second.key.baseKey);
	mKeys.erase(it);
	return true;
}

std::vector<uint64_t> SFrameReceiveKeyProvider::keyGenerations() const {
	const uint8_t bits = ratchetStepBits();

	std::shared_lock lock(mKeysMutex);
	std::vector<uint64_t> generations;
	generations.reserve(mKeys.size());
	// Reported as KIDs, as everything else here takes them, so a caller can hand one back.
	for (const auto &entry : mKeys)
		generations.push_back(impl::sframe::MakeKid(entry.first, 0, bits));

	return generations;
}

optional<SFrameReceiveKey> SFrameReceiveKeyProvider::receiveKey(uint64_t kid) const {
	const uint64_t keyGeneration = impl::sframe::KeyGenerationFromKid(kid, ratchetStepBits());

	std::shared_lock lock(mKeysMutex);
	auto it = mKeys.find(keyGeneration);
	if (it == mKeys.end())
		return nullopt;

	return it->second.key;
}

uint64_t SFrameReceiveKeyProvider::chainStepFor(uint64_t kid) const {
	const uint64_t keyGeneration = impl::sframe::KeyGenerationFromKid(kid, ratchetStepBits());

	std::shared_lock lock(mKeysMutex);
	auto it = mKeys.find(keyGeneration);
	return it != mKeys.end() ? it->second.chainStep : 0;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
