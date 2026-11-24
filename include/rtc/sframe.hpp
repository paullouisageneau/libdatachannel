/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_H
#define RTC_SFRAME_H

#if RTC_ENABLE_MEDIA

#include "message.hpp"
#include "rtp.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace rtc {

namespace impl {
class SFrameEncoder;
class SFrameDecoder;
class SFrameDecoderSet;
} // namespace impl

/// Send side encryption key material and info.
struct SFrameSendKey {
	binary baseKey;

	/// Full KID as the key management system issued it. RFC 9605 Section 5.1:
	///     kid = (key_generation << R) + (ratchet_step % (1 << R))
	uint64_t kid = 0;

	/// First counter for this key. Non-reuse of (key, CTR) is the application's responsibility:
	/// when resuming a key this MUST exceed every counter already used, which
	/// SFrameSendKeyProvider::currentKey() reports where there is one.
	///
	/// With per-SSRC derivation on there is no single counter to carry forward, so resume by
	/// rolling to a key generation never used before and leaving this at 0: the KID is an HKDF
	/// input to both the key and the salt, so a new generation is a fresh nonce space.
	uint64_t ctrStart = 0;
};

/// Receive side decryption key material
struct SFrameReceiveKey {
	/// Master key only: per-SSRC derivation occurs internally if used.
	binary baseKey;
};

/// The key generation takes whatever bits the ratchet step does not, so the step is capped well
/// short of the 64-bit KID.
inline constexpr uint8_t SFrameMaxRatchetStepBits = 32;

/// Protocol settings both ends must agree on. Chosen by the application layer.
class RTC_CPP_EXPORT SFrameKeyProvider {
public:
	virtual ~SFrameKeyProvider() = default;

	uint16_t cipherSuiteId() const { return mCipherSuiteId; }
	uint8_t ratchetStepBits() const { return mRatchetStepBits; }
	bool usePerSSRCDerivation() const { return mPerSsrcDerivation; }

protected:
	/// @param cipherSuiteId Suite id from the IANA SFrame registry at
	///                      https://www.iana.org/assignments/sframe, which names the defining
	///                      document for each. Not negotiated in SDP, and private suites are not
	///                      supported here.
	/// @param ratchetStepBits R in the KID layout, at most SFrameMaxRatchetStepBits. 0 does not
	///                        ratchet, which is what the Section 5.2 MLS layout uses: an MLS
	///                        epoch exports its own key rather than deriving from the last one.
	/// @param perSsrcDerivation Whether draft-ietf-avtcore-rtp-sframe Section 7 applies. Not
	///                          negotiated in SDP.
	///
	/// All three must match at both ends. None is negotiated, and a mismatch shows only as frames
	/// failing to authenticate, so they belong with the key material in whatever the application
	/// uses to agree keys.
	///
	/// Throws std::invalid_argument for a cipher suite outside the registry, for ratchetStepBits
	/// above SFrameMaxRatchetStepBits, and for 1 or 2, which are too narrow for the receiver to
	/// unwrap a wrapped step against its chain head -- use 0 for no ratcheting, or at least 3.
	SFrameKeyProvider(uint16_t cipherSuiteId, uint8_t ratchetStepBits, bool perSsrcDerivation);

private:
	const uint16_t mCipherSuiteId;
	const uint8_t mRatchetStepBits;
	const bool mPerSsrcDerivation;
};

/// Holds the sending side encryption key and configuration for a session.
class RTC_CPP_EXPORT SFrameSendKeyProvider : public SFrameKeyProvider {
public:
	/// @param ratchetPeriod Shortest interval in seconds between ratchets within one generation, up
	///                      to about 18 hours; 0 never ratchets, and a non-zero period with
	///                      ratchetStepBits 0 is rejected
	///
	/// Ratcheting is driven by outgoing frames, not by a timer: the period is a rate limit on a
	/// step the next frame takes, so an m-line that stops sending stops ratcheting, and the first
	/// frame after a long silence advances one step rather than the number of periods that passed.
	/// The schedule belongs to the provider rather than to a stream, so any SSRC that sends
	/// advances the step for its siblings and they catch up on their own next frame -- which is
	/// what keeps every stream on one m-line at the same KID, per
	/// draft-ietf-avtcore-rtp-sframe-02 Section 8. rollKey() restarts the schedule, so a provider
	/// re-keyed more often than ratchetPeriod never ratchets at all.
	///
	/// Throws std::invalid_argument for everything the base constructor rejects, for a base key
	/// under 16 bytes, and for a non-zero ratchetPeriod with ratchetStepBits 0 -- with no ratchet
	/// field the ratcheted key would be published under the KID it replaced, leaving the receiver
	/// no way to tell that the key changed.
	SFrameSendKeyProvider(uint16_t cipherSuiteId, uint8_t ratchetStepBits, uint16_t ratchetPeriod,
	                      bool perSsrcDerivation, SFrameSendKey key);
	~SFrameSendKeyProvider();

	/// Moves to new key material, for a group re-key or an MLS epoch change -- as distinct from
	/// ratcheting, which advances within one generation and needs no call. Takes effect on the
	/// next frame, and every encoder sharing this provider rolls together.
	///
	/// The ratchet schedule restarts here: the new generation begins at step 0 with a fresh period.
	/// A provider rolled more often than ratchetPeriod therefore never ratchets, since the timer is
	/// reset before it can fire.
	///
	/// The receiver must keep answering for the previous generation until frames under it have
	/// drained, or whatever was in flight is lost.
	///
	/// Re-supplying the key already in force -- the same KID and the same base key -- does nothing
	/// and is not a roll. Adopting a key restarts the counter at ctrStart, which under an unchanged
	/// key and KID would repeat every nonce already emitted, so this call is idempotent instead.
	///
	/// A KID this provider has already sent under is otherwise accepted, so long as the key
	/// material is new. KID lifecycle is the application's to manage, and the RFC 9605 Section 5.2
	/// MLS layout needs it: that KID carries only the low bits of the MLS epoch, so its values
	/// necessarily recur, and each recurrence brings a key the MLS exporter has freshly derived.
	/// Reusing a KID with new material is safe because the KID is an HKDF input, so the salt and
	/// content key change with it and no nonce repeats.
	///
	/// What is not safe, and is the application's to prevent, is re-supplying a key already sent
	/// under with a counter at or below one already spent. Resume a key part way along by
	/// constructing a provider with what currentKey() reported, rather than rolling to it -- see
	/// SFrameSendKey::ctrStart.
	///
	/// Throws std::invalid_argument for a base key under 16 bytes.
	void rollKey(SFrameSendKey key);

	/// How far sending has got: the KID in force, which advances as the key ratchets, and the
	/// counter the next frame will use.
	///
	/// Safe to persist and hand back only after sending has stopped. RFC 9605 Section 9.1 wants the
	/// next counter written to storage *before* it is used, so that losing the write cannot replay
	/// a counter; this reports where sending has reached, which is the other order. So a value
	/// sampled while frames are still going out is already stale, and resuming from one saved
	/// before an unclean stop repeats every counter spent after the sample -- under the same key
	/// and KID, which is a nonce collision. After a crash, roll to a key generation never used
	/// before rather than resuming.
	///
	/// nullopt with per-SSRC derivation on, where the counter belongs to each track's derived key
	/// rather than to the session. Resume that mode by rolling to an unused key generation
	/// instead -- see SFrameSendKey::ctrStart.
	optional<SFrameSendKey> currentKey() const;

	uint16_t ratchetPeriod() const { return mRatchetPeriod; }

private:
	friend class impl::SFrameEncoder;

	// The key as rollKey() last stored it, with the KID and counter as they were given rather
	// than as sending has advanced them, paired with the version it was stored at. The encoder
	// reads the version per frame and only copies the key when it changes.
	//
	// Both come from one locked read. Sampling the version separately can observe a roll between
	// the two, record the pre-roll version against the post-roll key, and so make the encoder adopt
	// that same key again on the next frame -- restarting its counter and repeating every nonce
	// already emitted under it.
	struct StoredKey {
		SFrameSendKey key;
		uint64_t version = 0;
	};
	StoredKey storedKey() const;
	uint64_t keyVersion() const { return mKeyVersion.load(std::memory_order_acquire); }

	// The ratchet step every encoder on this provider should be at, advanced by one when the period
	// has elapsed and a frame is going out. Shared rather than kept per encoder so that all the
	// streams of an m-line carry one KID, as draft-ietf-avtcore-rtp-sframe Section 8 describes --
	// the derived keys stay per SSRC, only the step is common. Any stream's frame advances it for
	// all of them; with nothing sending at all it does not advance, since a ratchet bounds how much
	// media a compromised key covers and silence produces none.
	uint64_t advanceRatchetStep();

	// Walks every encoder but the sender up to the shared step, so a paused stream does not pay for
	// the periods it missed. Once per step, and nothing at all with per-SSRC derivation off.
	void catchUpOtherEncoders(optional<SSRC> sending);

	const uint16_t mRatchetPeriod;
	uint64_t mRatchetStep = 0;                          // guarded by mKeyMutex
	uint64_t mSweptStep = 0;                            // guarded by mKeyMutex
	std::chrono::steady_clock::time_point mLastRatchet; // guarded by mKeyMutex

	mutable std::mutex mKeyMutex;
	SFrameSendKey mKey;
	std::atomic<uint64_t> mKeyVersion{0};

	// With per-SSRC derivation off every track sends under one key, so they must share its counter
	// and therefore one encoder, built here and handed to each. Null when derivation is per-SSRC
	// and every track has its own key and counter.
	shared_ptr<impl::SFrameEncoder> mSharedEncoder;

	// One encoder per SSRC with per-SSRC derivation on, so two packetizers on one SSRC share its
	// counter instead of each restarting at ctrStart. Each encoder holds this provider raw, since
	// the provider owns it. Uncapped: evicting one would lose its counter.
	mutable std::mutex mSsrcEncodersMutex;
	std::map<SSRC, shared_ptr<impl::SFrameEncoder>> mSsrcEncoders;
};

/// Holds the decryption keys for a session, one per key generation. Receiving is a lookup keyed
/// on a KID off the wire, so unlike the send side this holds several at once -- and must hold
/// both across a key roll, since a sender that rolls to N+1 still has N frames in flight.
///
/// Key lifetime is entirely the application's: a generation stays until removeKey() drops it.
/// Nothing is retired automatically, because deciding that a generation is finished with needs
/// knowledge this class does not have. It sees a KID and an SSRC, and neither carries the meaning
/// the decision needs -- SFrame leaves RTP headers rewritable so a relay can forward, so the SSRC
/// does not identify a sender; and key generations have no order to reason from, since the RFC 9605
/// Section 5.2 MLS layout carries only the low bits of an epoch and its values recur by design.
/// RFC 9605 Section 5.1 asks for a retired key to be deleted promptly; keyAuthenticated() and
/// keyGenerations() are what an application drives that from.
class RTC_CPP_EXPORT SFrameReceiveKeyProvider : public SFrameKeyProvider {
public:
	/// The three settings must match the sender's; see SFrameKeyProvider.
	///
	/// Throws std::invalid_argument for everything the base constructor rejects.
	SFrameReceiveKeyProvider(uint16_t cipherSuiteId, uint8_t ratchetStepBits,
	                         bool perSsrcDerivation)
	    : SFrameKeyProvider(cipherSuiteId, ratchetStepBits, perSsrcDerivation) {}

	/// Clears the key material it still holds.
	~SFrameReceiveKeyProvider() override;

	/// Adds the key for a KID's generation, or replaces what is held for it -- which revokes the
	/// old material at once, since the decoder consults this every frame rather than its cache.
	///
	/// The KID's ratchet step says where on the chain this key sits, so a key already advanced some
	/// way along can be registered as it is: pass what SFrameSendKeyProvider::currentKey() reported
	/// and a joiner picks the stream up mid-session. Frames at an earlier step than the registered
	/// one cannot be decoded, since a chain only runs forwards. Pass a step of 0 to register a
	/// generation's root key.
	///
	/// With per-SSRC derivation the ratchet advances the derived key rather than the base key
	/// (draft-ietf-avtcore-rtp-sframe Section 8), so an advanced key belongs to one SSRC and cannot
	/// be registered here, which is per generation. The step is ignored in that mode and the key
	/// taken as the generation's root, with a warning: a caller holding a KID off the wire should
	/// not have to mask the step off first, and currentKey() reports nullopt there, so no advanced
	/// key comes from this library to hand back.
	///
	/// A key supplied by a receiveKey() override instead of this call is taken as the generation's
	/// root, since there is no KID alongside it to say otherwise.
	///
	/// Throws std::invalid_argument for a base key under 16 bytes.
	void addKey(uint64_t kid, SFrameReceiveKey key);

	/// Drops the generation a KID belongs to. RFC 9605 Section 5.1 asks for a retired key to be
	/// deleted promptly -- promptly after frames under it have drained, not at the moment of the
	/// roll, or whatever was in flight is lost.
	/// @return true if that generation was held
	bool removeKey(uint64_t kid);

	/// The generations held, lowest first, each named by its KID at ratchet step 0, so they go
	/// straight back into removeKey().
	std::vector<uint64_t> keyGenerations() const;

	/// The master key for a KID's generation, or nullopt when this provider holds none, which makes
	/// the decoder refuse the frame. `kid` is the full KID off the wire.
	///
	/// Virtual as an escape hatch for keys that cannot be materialised ahead of time, such as an
	/// MLS exporter. Called on the media thread, and one provider may serve several tracks, so an
	/// override holding mutable state must be thread safe; the default is.
	virtual optional<SFrameReceiveKey> receiveKey(uint64_t kid) const;

	/// Called when a frame authenticates under a key generation other than the last one that did,
	/// including the first frame of a session. This is the signal to drive key lifetime from.
	///
	/// Not exactly-once: a generation is reported again whenever authentication alternates back to
	/// it -- an RTX straggler under the old key after a roll, or a decoder evicted from the set and
	/// recreated. So it does not mean the previous generation is finished with, and one report is
	/// not grounds for dropping anything. With the per-SSRC derivation on there is a decoder per
	/// stream, so several streams on one provider each report the same change; without it one
	/// decoder serves the m-line and the change is reported once.
	virtual void keyAuthenticated(uint64_t keyGeneration) { (void)keyGeneration; }

private:
	friend class impl::SFrameDecoder;

	// Where on the ratchet chain a registered key sits, taken from the KID's step field. Zero for a
	// generation's root key, which is the ordinary case.
	struct StoredReceiveKey {
		SFrameReceiveKey key;
		uint64_t chainStep = 0;
	};

	// The step recorded for a KID's generation, or 0 if none was registered -- which is also what a
	// receiveKey() override yields, having supplied no KID to read a step from.
	uint64_t chainStepFor(uint64_t kid) const;

	mutable std::shared_mutex mKeysMutex;
	std::map<uint64_t, StoredReceiveKey> mKeys;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_SFRAME_H
