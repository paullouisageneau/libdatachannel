/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_SFRAME_CODEC_H
#define RTC_IMPL_SFRAME_CODEC_H

#if RTC_ENABLE_MEDIA

#include "rtc/common.hpp"
#include "rtc/message.hpp"
#include "rtc/rtp.hpp"
#include "rtc/sframe.hpp"

#include "sframeutility.hpp" // sframe::SecretBinary for the derived key material below

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace rtc::impl {

// Exported, unlike the rest of src/impl, because the test suite exercises these directly.
/// SFrame Encoder
class RTC_CPP_EXPORT SFrameEncoder {
public:
	/// Throws std::invalid_argument if keyProvider is null or its base key is below
	/// MinBaseKeySize(). The first derivation runs here, not on the first frame, so bad key
	/// material is reported to the caller rather than from the media path.
	SFrameEncoder(shared_ptr<SFrameSendKeyProvider> keyProvider);

	/// As above, also applying the draft-ietf-avtcore-rtp-sframe Section 7 per-SSRC derivation,
	/// when the provider asks for it, to every generation it supplies. Prefer ForTrack() in library
	/// code: it memoises per SSRC, so two packetizers on one SSRC share a counter.
	SFrameEncoder(shared_ptr<SFrameSendKeyProvider> keyProvider, SSRC ssrc);

	~SFrameEncoder() = default;

	/// The encoder for one track: the provider's shared one with per-SSRC derivation off, since one
	/// key means one counter, or a memoised per-SSRC one with it on. `ssrc` is ignored in the
	/// former.
	static shared_ptr<SFrameEncoder> ForTrack(const shared_ptr<SFrameSendKeyProvider> &keyProvider,
	                                          SSRC ssrc);

	/// Encrypts one whole frame, preserving its FrameInfo. `metadata` is additional authenticated
	/// data and may be empty.
	shared_ptr<Message> encodeFrame(const shared_ptr<Message> &frame, const binary &metadata);

	/// The ratcheted key, the KID naming it, and the counter the next frame will use, taken under
	/// one lock. Sampled separately they can straddle a ratchet and pair a key with a KID and
	/// counter from either side of it, which either derives a key the receiver never computes or
	/// -- worse -- pairs the old key with a restarted counter and replays its whole nonce space.
	SFrameSendKey currentState() const;

	/// The counter the next frame will use. Restarts on every KID change -- at 0 on a ratchet, at
	/// SFrameSendKey::ctrStart on a roll -- because the KID is an HKDF input to both the key and
	/// the salt, so a new KID is a fresh nonce space.
	uint64_t currentCounter() const;

	/// Ratchets since the current key was adopted, reset at construction and at every rollKey().
	/// Absolute, so unlike the KID's step field it does not wrap at 2^R.
	uint64_t ratchetCount() const;

	/// Walks this encoder up to the m-line's step without it having to send, so a paused stream
	/// does not pay for every period it missed on its first frame back.
	void catchUpToStep(uint64_t target);

private:
	friend class rtc::SFrameSendKeyProvider;

	// One KID's key material: the base key it came from and everything derived for it. Rebuilt on
	// every roll and every ratchet, since both change the KID, and only the nonce depends on the
	// counter -- so the HKDF chain stays off the per-frame path.
	struct DerivedKey {
		sframe::SecretBinary baseKey; // after the per-SSRC derivation, if the session asks for it
		uint64_t kid; // composed from the key generation and this key's ratchet step
		sframe::SecretBinary encryptionKey;
		sframe::SecretBinary salt;
		size_t nonceSize;
	};

	// For an encoder the provider owns: taken raw, since two shared_ptrs pointing at each other
	// would never be freed.
	SFrameEncoder(SFrameSendKeyProvider *keyProvider, optional<SSRC> ssrc);
	SFrameEncoder(shared_ptr<SFrameSendKeyProvider> keyProvider, optional<SSRC> ssrc);

	shared_ptr<const DerivedKey> deriveKey(const binary &baseKey, uint64_t kid) const;

	binary encodeSFrame(const DerivedKey &key, uint64_t ctr, const binary &plaintext,
	                    const binary &metadata);
	// The derived key and the counter to use with it, taken together under mKeyMutex: claimed
	// apart, a frame could pair a stale key with a counter already spent under it.
	struct FrameKey {
		shared_ptr<const DerivedKey> derived;
		uint64_t ctr = 0;
	};
	FrameKey claimFrameKey();

	// Rolls or ratchets if due without claiming a counter, so the constructor can establish the
	// first key without spending CTR 0 on nothing.
	void refreshKey();
	void refreshKeyLocked();
	// Walks to an absolute step, deriving once at the end. No-op when already there, so it leaves
	// the counter alone in that case.
	void ratchetTo(uint64_t target);
	void adoptSendKey(const SFrameSendKey &sendKey);

	// Replaced wholesale on ratchet, so a frame already encrypting keeps the key it started with:
	// mKeyMutex covers the swap, never the crypto.
	mutable std::mutex mKeyMutex;
	shared_ptr<const DerivedKey> mDerivedKey;

	// Counter for the current KID, guarded by mKeyMutex.
	uint64_t mCtr = 0;

	// Provider key version the key above was built from, compared per frame.
	uint64_t mKeyVersion = 0;

	uint64_t mRatchetCount = 0; // guarded by mKeyMutex

	// Set only by the shared_ptr constructors, which the library itself never uses -- ForTrack()
	// takes the provider raw, since the provider owns the encoder. mKeyProvider is what the code
	// reads either way.
	const shared_ptr<SFrameSendKeyProvider> mOwnedKeyProvider;
	SFrameSendKeyProvider *const mKeyProvider;

	const optional<SSRC> mSsrc;
};

/// SFrame Decoder
class RTC_CPP_EXPORT SFrameDecoder {
public:
	// How much ratchet catch-up work a peer may force, held apart from the decoder so a whole track
	// shares one allowance rather than one per SSRC.
	struct CatchUpLimiter {
		std::chrono::steady_clock::time_point lastDecodeOk{};
		std::chrono::steady_clock::time_point lastCatchUp{};

		// The furthest ratchet step that has authenticated on this m-line, per master key. The
		// sender advances the step for every stream together, so a stream returning from idle is
		// behind by exactly what the others have already proven, and walking it forward to meet
		// them is expected rather than speculative.
		//
		// Keyed on the master key, as chains are, and not on the key generation: addKey() may
		// replace a generation's material in place, and the fresh chain that follows starts at step
		// zero. A reference carried over from the old material would place its first frame at the
		// old chain's position.
		struct ProvenStep {
			sframe::SecretBinary masterKey;
			uint64_t step = 0;
		};

		// Defined here rather than out of line: a nested class is not covered by the enclosing
		// class's RTC_CPP_EXPORT, so out-of-line definitions link on every platform except MSVC,
		// where the test suite fails with an unresolved external.

		/// The furthest step proven for this master key, or 0 if none is held.
		uint64_t provenStep(const binary &masterKey) const {
			for (const auto &entry : mProvenSteps)
				if (sframe::ConstantTimeEquals(entry.masterKey.bytes(), masterKey))
					return entry.step;
			return 0;
		}

		/// Records a step as proven, keeping the most recently used first and evicting past the cap.
		/// Never lowers a step already held.
		void recordProven(const binary &masterKey, uint64_t step) {
			auto it = std::find_if(mProvenSteps.begin(), mProvenSteps.end(),
			                       [&](const ProvenStep &entry) {
				                       return sframe::ConstantTimeEquals(entry.masterKey.bytes(),
				                                                         masterKey);
			                       });

			if (it == mProvenSteps.end()) {
				mProvenSteps.insert(mProvenSteps.begin(), {masterKey, step});
				if (mProvenSteps.size() > MaxProvenSteps)
					mProvenSteps.pop_back(); // the SecretBinary clears the key it drops
				return;
			}

			// Never lowers: a straggler authenticating at an older step must not retract a
			// reference a later frame already established, or an idle sibling loses the exemption
			// it is owed.
			it->step = std::max(it->step, step);
			if (it != mProvenSteps.begin())
				std::rotate(mProvenSteps.begin(), it, it + 1); // keep most recently used first
		}

	private:
		// A handful of keys are live at once; the reference is an optimisation, so dropping one
		// only costs a fall back to the rate-limited walk. constexpr so it needs no out-of-line
		// definition either.
		static constexpr size_t MaxProvenSteps = 8;

		std::vector<ProvenStep> mProvenSteps; // most recently used first
	};

	/// Decode against the provider's key material as-is, with a catch-up allowance of its own.
	/// Pairs with SFrameEncoder.
	SFrameDecoder(const shared_ptr<SFrameReceiveKeyProvider> &keyProvider)
	    : mKeyProvider(keyProvider), mLimiter(std::make_shared<CatchUpLimiter>()) {}

	/// Decode every stream on an m-line with one decoder, sharing the track's catch-up allowance.
	/// For sessions without the per-SSRC derivation, where the SSRC is no part of the key schedule,
	/// so one chain serves them all and none can fall behind the others.
	SFrameDecoder(const shared_ptr<SFrameReceiveKeyProvider> &keyProvider,
	              shared_ptr<CatchUpLimiter> limiter)
	    : mKeyProvider(keyProvider),
	      mLimiter(limiter ? std::move(limiter) : std::make_shared<CatchUpLimiter>()) {}

	/// Decode frames arriving on one SSRC, applying the per-SSRC key derivation when the provider
	/// asks for it. Pairs with SFramePerFrameRtpPacketizer.
	/// @param ssrc RTP SSRC these frames arrive on
	/// @param limiter Catch-up allowance to share with the track's other decoders; a decoder given
	///                none keeps its own
	SFrameDecoder(const shared_ptr<SFrameReceiveKeyProvider> &keyProvider, SSRC ssrc,
	              shared_ptr<CatchUpLimiter> limiter = nullptr)
	    : mKeyProvider(keyProvider), mSsrc(ssrc),
	      mLimiter(limiter ? std::move(limiter) : std::make_shared<CatchUpLimiter>()) {}

	~SFrameDecoder() = default;

	shared_ptr<Message> decodeFrame(const shared_ptr<Message> &encodedFrame,
	                                const binary &metadata);

private:
	binary decodeSFrame(const binary &packet, const binary &metadata);

	// Key material for one KID, cached between frames: only the nonce depends on the counter.
	struct DerivedKey {
		uint64_t kid;
		// Absolute ratchet step. The KID alone does not identify the key: its step field is
		// only R bits wide, so a KID recurs every 2^R ratchets naming different material.
		uint64_t step;
		sframe::SecretBinary baseKey; // master key this was derived from, for invalidation
		sframe::SecretBinary encryptionKey;
		sframe::SecretBinary salt;
	};

	struct CachedKey {
		shared_ptr<const DerivedKey> key;
		std::chrono::steady_clock::time_point lastUsed;
	};

	// A key for one frame, not yet trusted: the step comes off the wire, so it is not applied until
	// the frame authenticates, or a forged step would drag the chain past the real sender.
	struct KeyLookup {
		shared_ptr<const DerivedKey> derived;
		bool fromCache = false;
		bool advancesChain = false;
		uint64_t step = 0;
		sframe::SecretBinary chainKey;
	};

	// Ratchet chain for one master key, kept per key rather than per decoder: the generation naming
	// it is unauthenticated, so a chain must never be torn down to serve a frame that is forged.
	struct Chain {
		sframe::SecretBinary masterKey; // provider's base key this chain came from
		sframe::SecretBinary root;      // once-only Section 7 per-SSRC derivation
		sframe::SecretBinary key;       // the key for `step`, walked forward from root
		uint64_t step = 0;
		bool established = false; // a frame on this chain has authenticated
	};

	uint64_t unwrapRatchetStep(uint64_t wireStep, uint8_t ratchetStepBits,
	                           uint64_t chainStep) const;
	Chain &selectChain(const SFrameReceiveKey &receiveKey, uint64_t registeredStep);
	KeyLookup lookupKey(uint64_t kid, uint8_t ratchetStepBits, const SFrameReceiveKey &receiveKey);
	void commitKey(KeyLookup &lookup);
	void expireCachedKeys();

	// A track has one live KID, two across a ratchet, and only accepted KIDs are cached.
	inline static const size_t MaxCachedKeys = 8;

	// Generous next to real RTP reordering, short enough that a ratcheted-away key does not linger.
	// RFC 9605 Section 5.1: a retired key "should be deleted promptly".
	inline static const std::chrono::seconds KeyRetention{30};

	// A receiver keeping up is a ratchet or two behind, and each step costs an HKDF chain.
	inline static const uint64_t MaxForwardRatchet = 2;

	// A joiner starts wherever a long-running session has reached, so the chain gets one
	// fast-forward. Spent on the attempt, before the tag is checked, so a forged step can consume
	// it and delay a genuine joiner by up to CatchUpInterval -- deliberately, since charging it
	// only on success would let every forged packet force a full walk of HKDF rounds.
	inline static const uint64_t MaxInitialRatchet = 2048;

	// A long walk runs at most once per CatchUpInterval, and on an established chain only once the
	// last successful decode is CatchUpAfter old, so a receiver that went quiet can rejoin.
	inline static const std::chrono::seconds CatchUpInterval{1};
	inline static const std::chrono::seconds CatchUpAfter{30};

	// One chain per live master key; a provider holds two or three, so eviction only ever discards
	// material a peer named and stopped using.
	inline static const size_t MaxChains = 4;

	const shared_ptr<SFrameReceiveKeyProvider> mKeyProvider;
	const optional<SSRC> mSsrc;
	std::vector<CachedKey> mDerivedKeys; // most recent first
	std::vector<Chain> mChains;          // most recently used first

	// Generation of the last frame that authenticated, so keyAuthenticated() fires on a change.
	optional<uint64_t> mLastAuthenticatedGeneration;

	// Shared by every chain: per-chain, alternating two generations would re-arm a long walk on
	// every forged frame.
	const shared_ptr<CatchUpLimiter> mLimiter;
};

// Holds the decoders for one m-line. Every SSRC of an m-line reaches one depacketizer, so simulcast,
// RTX and FEC all land here.
//
// With the per-SSRC derivation on, a decoder's chain and derived keys hang off its SSRC, so there is
// one per stream: sharing would clear all of it on every alternation, recoverable only by the
// rate-limited catch-up walk. With the derivation off the SSRC is no part of the key schedule, so
// the streams share one master key, one sender-side ratchet and therefore one chain -- and one
// decoder serves them all. Splitting there would have each stream maintain a byte-identical chain
// off its own traffic, so a stream that went quiet would fall behind and then be refused the
// catch-up needed to rejoin, the busy streams having kept the shared allowance looking live.
class RTC_CPP_EXPORT SFrameDecoderSet {
public:
	/// Whether the SSRC passed to DecryptMessages() selects anything, and so whether a batch has
	/// to be split by it.
	bool usePerSSRCDerivation() const { return mKeyProvider->usePerSSRCDerivation(); }

	/// Throws std::invalid_argument if keyProvider is null. Checked here rather than by the
	/// callers, so a set without a provider cannot be constructed -- every decoder it hands out
	/// would dereference it.
	explicit SFrameDecoderSet(shared_ptr<SFrameReceiveKeyProvider> keyProvider);

	// Spelt out because the decoder map is move-only, so an implicitly declared copy constructor
	// would instantiate std::map's -- which MSVC defines for an exported class whether or not
	// anything calls it, and that fails to compile.
	SFrameDecoderSet(const SFrameDecoderSet &) = delete;
	SFrameDecoderSet &operator=(const SFrameDecoderSet &) = delete;
	SFrameDecoderSet(SFrameDecoderSet &&) = delete;
	SFrameDecoderSet &operator=(SFrameDecoderSet &&) = delete;

	// Created on first sight, sharing the track's catch-up allowance so cycling SSRCs buys nothing.
	SFrameDecoder &forSsrc(SSRC ssrc);

private:
	const shared_ptr<SFrameReceiveKeyProvider> mKeyProvider;
	const shared_ptr<SFrameDecoder::CatchUpLimiter> mLimiter;

	struct Entry {
		unique_ptr<SFrameDecoder> decoder;
		uint64_t lastUse = 0;
	};
	// Keyed by SSRC with per-SSRC derivation on. With it off the map holds a single entry under
	// kSharedDecoderKey, since the SSRC is no part of the key schedule then.
	std::map<SSRC, Entry> mDecoders;
	inline static const SSRC kSharedDecoderKey = 0;
	uint64_t mUses = 0;

	// The peer decides how many SSRCs appear, so the stream idle longest is evicted; losing a
	// decoder costs only its chain, which the catch-up walk rebuilds.
	//
	// The same constant the depacketizers cap their reassembly tables with, not a copy of its
	// value: both count SSRCs on one m-line, so a number that is right for one is right for the
	// other, and the two drifting apart is a live bug rather than a hypothetical -- a reassembly
	// table smaller than the decoder table thrashes on exactly the wide m-line the larger number
	// was chosen for. Sized well above any realistic m-line (simulcast layers plus their RTX and
	// FEC streams, or an SFU forwarding several participants) so eviction is not reached in
	// practice.
	inline static const size_t MaxDecoders = sframe::MaxSFrameStreams;
};

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA

#endif // RTC_IMPL_SFRAME_CODEC_H
