/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_H
#define RTC_SFRAME_H

#include "message.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

// SFrame RFC payload descriptor bits (draft-ietf-avtcore-rtp-sframe section 4).
// Layout is |S E T x x x x x|: S marks the first packet of an object, E the last, and T the
// payload origin (0 raw, 1 packetized). The draft reserves the 5 remaining bits for future
// use without saying what to do with them set; this implementation requires them zero and
// drops the packet otherwise. Only T=0 is produced or accepted: per-packet SFrame is not
// implemented.
#define SFRAME_DESCRIPTOR_S 0x80
#define SFRAME_DESCRIPTOR_E 0x40
#define SFRAME_DESCRIPTOR_T 0x20

namespace rtc {

/// What a base key may be used for. RFC 9605 Section 4.4.1: "Implementations MUST mark each
/// base_key as usable for encryption or decryption, never both."
enum class SFrameKeyUse {
	/// This endpoint sends under this key. No other sender may use it: two senders on one key
	/// both start at ctrStart and reuse every nonce, which on the GCM suites leaks the
	/// authentication key and on the CTR suites exposes the plaintexts to an XOR.
	Encrypt,

	/// This endpoint receives under this key, which some other endpoint sends under.
	Decrypt,
};

/// Key material for one key generation. The KID is composed and parsed by the library, so
/// nothing here names a ratchet step.
struct SFrameKeyDetails {
	/// Cipher suite, RFC 9605 Section 8.1. 0 is unregistered and is rejected.
	uint16_t cipherSuiteId = 0;
	binary baseKey;
	/// First counter used with this key. Non-reuse of (key, CTR) is the sender's obligation
	/// (RFC 9605 Section 9.1) and the counter is not persisted: when reusing a base key
	/// across processes this MUST exceed every counter already spent on it, or the previous
	/// session's nonce sequence is replayed. See SFrameEncoder::currentCounter().
	uint64_t ctrStart = 0;

	/// Which direction this key is for. SFrameEncoder refuses a key not marked Encrypt and
	/// SFrameDecoder refuses one not marked Decrypt, so a key cannot silently serve both --
	/// the two directions of a call need separate base keys, and separate key generations to
	/// name them by.
	///
	/// Deliberately unset by default and rejected if left unset: the library cannot infer the
	/// role, and getting it wrong is what produces a repeated (key, nonce) pair.
	std::optional<SFrameKeyUse> use;
};

enum class SFrameMode {
	/// One object per frame, fragmented across RTP packets, descriptor T=0. Section 5.1.1.
	PerFrame,

	/// One object per RTP packet, descriptor T=1. Sections 5.1.2 and 5.2.
	// PerPacket, // Not currently implemented
};

struct SFrameConfig {
	SFrameKeyDetails keyDetails;

	/// Key generation this sender's KIDs name. The library composes the KID from it,
	/// RFC 9605 Section 5.1:  KID = (keyGeneration << R) + (ratchet_step % (1 << R))
	uint64_t keyGeneration = 0;

	/// Ratchet step this sender starts at. Like ctrStart, this is for resuming a key across
	/// process lifetimes: the receiver walks forward to meet it.
	uint64_t ratchetStep = 0;

	/// Ratchet step bits R: the low KID bits the ratchet step occupies. The step wraps at
	/// 2^R, so R bounds how many may be active at once. 0 means not ratcheted, and is
	/// rejected alongside a non-zero ratchet period. At most MaxRatchetStepBits.
	uint8_t ratchetStepBits = 0;

	uint64_t ratchetPeriod = 0; // Seconds between key ratchets; 0 means never ratchet

	/// Whether SFrameRtpPacketizer applies the per-SSRC key derivation of
	/// draft-ietf-avtcore-rtp-sframe Section 7 on top of the base key. It is a MAY in the
	/// draft and is not negotiated in SDP, so it is a deployment setting like the cipher
	/// suite: both ends are configured with the same answer, or every frame authenticates
	/// against the wrong key with no other symptom. Must match the receiving provider's
	/// SFrameKeyProvider::usePerSSRCDerivation().
	///
	/// Deliberately unset by default and rejected if left unset, since either answer is
	/// wrong against half the peers. Ignored by SFrameEncoder, which takes whatever key it
	/// is given, and by the SFrameRtpPacketizer constructor that takes a prebuilt encoder.
	///
	/// With derivation off every track shares one key and therefore one counter, so a second
	/// track on the same key MUST take the shared-encoder constructor rather than another
	/// copy of this config: two encoders would both start at ctrStart and reuse every nonce.
	std::optional<bool> perSsrcDerivation;

	/// The KID is 64 bits and the key generation takes whatever the step does not, so the
	/// step is capped well short of the field.
	static constexpr uint8_t MaxRatchetStepBits = 32;
};

/// SFrame Encoder
class RTC_CPP_EXPORT SFrameEncoder  {
public:
	/// Throws std::invalid_argument for a base key below MinBaseKeySize(), for
	/// ratchetStepBits above MaxRatchetStepBits, or for a ratchet period without a
	/// ratchet field.
	/// @param config Key material, key generation and ratchet settings
	SFrameEncoder(const SFrameConfig &config);
	virtual ~SFrameEncoder() {}

	/// Encrypts one whole frame, preserving its FrameInfo.
	/// @param frame Plaintext frame
	/// @param metadata Additional authenticated data, may be empty
	std::shared_ptr<Message> encodeFrame(const std::shared_ptr<Message>& frame, const binary& metadata);

	/// The counter the next frame will use. Persist it alongside the base key and hand it
	/// back as SFrameKeyDetails::ctrStart to continue a key without reusing a nonce.
	uint64_t currentCounter() const { return mCtr.load(std::memory_order_relaxed); }

private:
	// One key generation and everything derived from it. Only the nonce depends on the
	// frame counter, so the HKDF chain runs once here rather than per frame.
	struct KeyEpoch {
		SFrameKeyDetails details;
		uint64_t kid; // composed from the key generation and this epoch's ratchet step
		binary encryptionKey;
		binary salt;
		size_t nonceSize;
	};

	static std::shared_ptr<const KeyEpoch> makeEpoch(const SFrameKeyDetails& details, uint64_t kid);

	binary encodeSFrame(const KeyEpoch& epoch, uint64_t ctr, const binary& plaintext, const binary& metadata);
	std::shared_ptr<const KeyEpoch> currentEpoch();
	bool shouldRatchetKey();
	void ratchetKey();

	// An immutable snapshot, replaced wholesale on ratchet, so a frame already encrypting
	// keeps the one it started with. mKeyMutex covers only the swap, never the crypto.
	std::mutex mKeyMutex;
	std::shared_ptr<const KeyEpoch> mKeyEpoch;
	std::chrono::steady_clock::time_point mLastRatchet;

	// Claimed with fetch_add so concurrent senders can never be handed the same counter.
	std::atomic<uint64_t> mCtr;
	std::chrono::seconds mRatchetPeriod;
	const uint8_t mRatchetStepBits;
};

/// SFrame Key Provider, needs to be passed to the decoder
class RTC_CPP_EXPORT SFrameKeyProvider {
public:
	virtual ~SFrameKeyProvider() = default;

	/// The master key for a key generation, or nullopt when it is not one this provider
	/// knows -- which is what makes the decoder refuse the frame. Never substitute a
	/// placeholder: whatever is returned decrypts the frame, so a key an attacker can also
	/// derive turns any KID they invent into an authenticated frame. Return the master key,
	/// not a per-stream one: the library applies the per-SSRC derivation of
	/// draft-ietf-avtcore-rtp-sframe Section 7 itself, and has already stripped the ratchet
	/// step, so this never sees a raw KID. One provider may serve several tracks on
	/// independent threads, so an implementation holding mutable state must be thread safe.
	virtual std::optional<SFrameKeyDetails> getKeyDetails(uint64_t keyGeneration) = 0;

	/// Whether the library applies the per-SSRC key derivation of
	/// draft-ietf-avtcore-rtp-sframe Section 7 on top of the base key. It is a MAY in the
	/// draft, so this must match what the sender does: derive when it did not, or fail to,
	/// and every frame authenticates against the wrong key with no other symptom.
	///
	/// Returning false interoperates with a plain RFC 9605 peer. It also means every track
	/// shares one key, so the sending side must share one SFrameEncoder across them --
	/// see the SFrameRtpPacketizer constructor that takes one.
	///
	/// Deliberately pure: there is no safe default, since either answer is wrong against
	/// half the peers.
	virtual bool usePerSSRCDerivation() const = 0;

	/// Ratchet step bits R for this session's KID layout (RFC 9605 Section 5.1). The library
	/// needs it to split an incoming KID, so it is asked once per lookup rather than being
	/// returned with the key. 0 means the peer does not ratchet.
	///
	/// A provider using the Section 5.2 MLS layout reports 0 and receives the whole KID as
	/// the key generation, splitting context, sender_index and epoch itself. Its epoch field
	/// is not a ratchet step: each MLS epoch exports its own base key rather than deriving it
	/// from the previous one, so reporting E here would have the library ratchet forward from
	/// the wrong key.
	virtual uint8_t ratchetStepBits() const { return 0; }
};

/// SFrame Decoder
class RTC_CPP_EXPORT SFrameDecoder  {
public:
	/// Decode against the provider's key material as-is. Pairs with SFrameEncoder.
	/// @param keyProvider Supplies per-KID key material
	SFrameDecoder(const std::shared_ptr<SFrameKeyProvider>& keyProvider)
	    : mKeyProvider(keyProvider) {}

	/// Decode frames arriving on one SSRC, applying the per-SSRC key derivation to the
	/// provider's master key. Pairs with SFrameRtpPacketizer.
	/// @param keyProvider Supplies per-KID key material
	/// @param ssrc RTP SSRC these frames arrive on
	SFrameDecoder(const std::shared_ptr<SFrameKeyProvider>& keyProvider, uint32_t ssrc)
	    : mKeyProvider(keyProvider), mSsrc(ssrc) {}

	virtual ~SFrameDecoder() {};

	// decode
	std::shared_ptr<Message> decodeFrame(const std::shared_ptr<Message>& encodedFrame, const binary& metadata);

	/// The SSRC this decoder derives for, or nullopt if it decodes the provider's key
	/// directly.
	std::optional<uint32_t> ssrc() const { return mSsrc; }

	/// Points the decoder at a new SSRC, discarding the key material derived for the old
	/// one. The catch-up rate limit is kept: a peer that changes SSRC every packet would
	/// otherwise get a fresh ratchet allowance each time.
	/// @param ssrc RTP SSRC subsequent frames arrive on
	void rebindSsrc(uint32_t ssrc);

private:
	binary decodeSFrame(const binary& packet, const binary& metadata);

	// Key material for one KID. Only the nonce depends on the frame counter, so this
	// survives between frames; deriving it costs up to two HKDF chains.
	struct DerivedKey {
		uint64_t kid;
		// Absolute ratchet step. The KID alone does not identify the key: its step field is
		// only R bits wide, so a KID recurs every 2^R ratchets naming different material.
		uint64_t step;
		uint16_t cipherSuiteId;
		binary baseKey; // master key this was derived from, for invalidation
		binary encryptionKey;
		binary salt;
	};

	struct CachedKey {
		std::shared_ptr<const DerivedKey> key;
		std::chrono::steady_clock::time_point lastUsed;
	};

	// A key for one frame, not yet trusted. The ratchet step comes off the wire, so it is
	// not applied until that frame authenticates: otherwise one forged frame naming a
	// distant step would drag the chain past the real sender.
	struct KeyLookup {
		std::shared_ptr<const DerivedKey> derived;
		bool fromCache = false;
		bool advancesChain = false;
		uint64_t step = 0;
		binary chainKey;
	};

	// Ratchet chain for one master key. Kept per key, not per decoder: the key generation
	// naming it comes off the wire before anything authenticates, so a chain must never be
	// torn down to serve a frame that turns out to be forged.
	struct Chain {
		binary masterKey; // provider's base key this chain came from
		uint16_t cipherSuiteId = 0;
		binary root; // once-only Section 8 derivation
		binary key;  // root ratcheted step times
		uint64_t step = 0;
		bool established = false; // a frame on this chain has authenticated
	};

	uint64_t unwrapRatchetStep(uint64_t wireStep, uint8_t ratchetStepBits,
	                           uint64_t chainStep) const;
	Chain &selectChain(const SFrameKeyDetails& keyDetails);
	KeyLookup lookupKey(uint64_t kid, uint8_t ratchetStepBits, const SFrameKeyDetails& keyDetails);
	void commitKey(KeyLookup& lookup);
	void expireCachedKeys();

	// A track has one live KID, two across a ratchet. Only KIDs the provider accepted are
	// cached, so a peer cannot grow it.
	static constexpr size_t MaxCachedKeys = 8;

	// Dropped once unused this long: generous next to real RTP reordering, short enough
	// that a ratcheted-away key does not linger. RFC 9605 Section 5.1 says the old key
	// "should be deleted promptly".
	static constexpr std::chrono::seconds KeyRetention{30};

	// A receiver keeping up is a ratchet or two behind, and each step costs an HKDF chain
	// driven by a step off the wire.
	static constexpr uint64_t MaxForwardRatchet = 2;

	// A joiner starts wherever a long-running session has reached, so the chain gets one
	// fast-forward; at a ratchet period of minutes this covers a session days old. Spent
	// on the first authenticated frame, so it cannot be replayed.
	static constexpr uint64_t MaxInitialRatchet = 2048;

	// A long walk is allowed at most this often, so a forged step cannot force one per
	// frame, and at most this long after the last successful decode, so a receiver that
	// went quiet can rejoin the chain.
	static constexpr std::chrono::seconds CatchUpInterval{1};
	static constexpr std::chrono::seconds CatchUpAfter{30};

	// One chain per live master key. A provider holds a generation or two, three across a
	// re-key, so this only ever evicts material a peer named and then stopped using.
	static constexpr size_t MaxChains = 4;

	const std::shared_ptr<SFrameKeyProvider> mKeyProvider;
	std::optional<uint32_t> mSsrc;
	std::vector<CachedKey> mDerivedKeys; // most recent first
	std::vector<Chain> mChains;          // most recently used first

	// Catch-up state. Shared by every chain: per-chain, alternating two key generations
	// would re-arm a long walk on every forged frame.
	std::chrono::steady_clock::time_point mLastDecodeOk{};
	std::chrono::steady_clock::time_point mLastCatchUp{};
};

} // namespace rtc

#endif // RTC_SFRAME_H
