/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_SFRAME_UTILITY_H
#define RTC_IMPL_SFRAME_UTILITY_H

#if RTC_ENABLE_MEDIA

#include "rtc/common.hpp"
#include "rtc/message.hpp"
#include "rtc/rtp.hpp"
#include "rtc/sframe.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rtc::impl {

// SFrame RFC payload descriptor bits (draft-ietf-avtcore-rtp-sframe section 4).
// Layout is |S E T x x x x x|: S marks the first packet of an object, E the last, and T the
// payload origin (0 raw, 1 packetized). The 5 remaining bits are reserved for future use, so
// they are sent as zero and ignored on receive: dropping a packet for setting one would fail
// against any future extension that uses them. Only T=0 is produced or accepted -- per-packet
// SFrame is not implemented, and T=1 is declined rather than mis-delivered.
constexpr uint8_t SFrameDescriptorS = 0x80;
constexpr uint8_t SFrameDescriptorE = 0x40;
constexpr uint8_t SFrameDescriptorT = 0x20;

struct SFrameKeyMaterial {
	binary contentEncryptionKey;
	binary nonce;
};

// The key material one KID derives to. Distinct from SFrameKeyMaterial, which carries a nonce for
// one counter rather than the salt every counter derives from.
struct SFrameKidKeys {
	binary encryptionKey;
	binary salt;
};

/// Decoded SFrame header fields. length is the encoded header size in bytes.
struct SFrameHeaderInfo {
	uint64_t kid;
	uint64_t ctr;
	size_t length;
};
/// SFrame header, RFC 9605 Section 4.3
// Exported, unlike the rest of src/impl, because the test suite exercises these directly.
namespace sframe::header {

RTC_CPP_EXPORT binary Encode(SFrameHeaderInfo headerInfo);
RTC_CPP_EXPORT SFrameHeaderInfo Decode(const binary &encodedFrame);

} // namespace sframe::header

/// SFrame utilities
// Exported, unlike the rest of src/impl, because the test suite exercises these directly.
namespace sframe {

RTC_CPP_EXPORT SFrameKeyMaterial GetSFrameKeys(uint64_t kid, uint64_t ctr, const binary &baseKey,
                                               uint16_t cipherSuite);
RTC_CPP_EXPORT binary MakeAAD(const binary &header, const binary &metadata);
RTC_CPP_EXPORT binary GetSFrameKeyLabel(uint64_t kid, uint16_t cipherSuite);
RTC_CPP_EXPORT binary GetSFrameSaltLabel(uint64_t kid, uint16_t cipherSuite);
RTC_CPP_EXPORT binary DeriveNonce(const binary &sframeSalt, uint64_t ctr, size_t nonceSize);

// pt -> ct
RTC_CPP_EXPORT binary EncodePlaintext(uint16_t cipherSuite, const binary &sframeKey,
                                      const binary &nonce, const binary &aad,
                                      const binary &plaintext);

// ct -> pt
RTC_CPP_EXPORT binary DecodeCiphertext(uint16_t cipherSuite, const binary &sframeKey,
                                       const binary &nonce, const binary &aad,
                                       const binary &cipherText);

RTC_CPP_EXPORT size_t MinimalByteLength(uint64_t v);
RTC_CPP_EXPORT binary EncodeBigEndian(uint64_t value, size_t numBytes);
RTC_CPP_EXPORT uint64_t DecodeBigEndian(const binary &buf, size_t offset, size_t len);

// KID = (keyGeneration << R) + (ratchetStep % (1 << R)), per RFC 9605 Section 5.1.
RTC_CPP_EXPORT uint64_t MakeKid(uint64_t keyGeneration, uint64_t ratchetStep,
                                uint8_t ratchetStepBits);

// The two halves of a KID. A receiver needs these to tell a ratchet step, which it can
// follow by ratcheting forward, from a new key generation, which it cannot.
RTC_CPP_EXPORT uint64_t KeyGenerationFromKid(uint64_t kid, uint8_t ratchetStepBits);
RTC_CPP_EXPORT uint64_t RatchetStepFromKid(uint64_t kid, uint8_t ratchetStepBits);

// The KID one ratchet step on, wrapping the step to zero and leaving the key
// generation untouched.
RTC_CPP_EXPORT uint64_t NextRatchetKid(uint64_t kid, uint8_t ratchetStepBits);

// A peer decides how many SSRCs appear on an m-line, so the per-stream table is capped. This is the
// one place that number is chosen: the audio and video depacketizers cap their reassembly tables
// with it and SFrameDecoderSet::MaxDecoders is defined as it, because all three count SSRCs on one
// m-line. Evicting a stream is the costliest of the three -- it drops the groups that stream had in
// flight, where evicting a decoder costs only a chain the catch-up walk rebuilds.
// The video depacketizer's MaxSFrameBufferedBytes is a total across streams, so raising this does
// not raise its ceiling; the audio one bounds MaxPartialSize per stream, so raising this does raise
// that worst case by one buffer per added stream.
const size_t MaxSFrameStreams = 32;

// Minimum base key size in bytes; see sframe_crypto::MinBaseKeySize.
RTC_CPP_EXPORT size_t MinBaseKeySize();

// Throws if the cipher suite is not in the SFrame registry.
RTC_CPP_EXPORT void ValidateCipherSuite(uint16_t cipherSuiteId);

// Compares key material without branching on its contents.
// Wraps sframe_crypto::ConstantTimeEquals without exposing internal crypto headers.
RTC_CPP_EXPORT bool ConstantTimeEquals(const binary &a, const binary &b);

// Overwrites key material with zeroes, leaving the buffer's length alone.
// Wraps sframe_crypto::Cleanse without exposing internal crypto headers.
RTC_CPP_EXPORT void Cleanse(binary &buffer);

// Key material that clears itself once nothing holds it, so a key does not outlive its use in
// freed heap. Assignment clears what it replaces, which is what covers ratcheting: every step
// overwrites a chain key, and a catch-up walk can drop thousands of them.
//
// Reaches only the bytes it owns. A key copied out through the public SFrameSendKey or
// SFrameReceiveKey belongs to the application, and growing the buffer would abandon the old
// block -- these are only ever assigned whole, never appended to.
class SecretBinary {
public:
	SecretBinary() = default;
	SecretBinary(binary bytes) : mBytes(std::move(bytes)) {}
	~SecretBinary() { Cleanse(mBytes); }

	// A moved-from vector is empty, so the source's wipe is a no-op and the bytes travel with the
	// heap block; only the assignments below have old material to clear first.
	SecretBinary(const SecretBinary &other) = default;
	SecretBinary(SecretBinary &&other) = default;

	SecretBinary &operator=(const SecretBinary &other) {
		if (this != &other)
			assign(other.mBytes);
		return *this;
	}

	SecretBinary &operator=(SecretBinary &&other) noexcept {
		if (this != &other)
			assign(std::move(other.mBytes));
		return *this;
	}

	SecretBinary &operator=(binary bytes) {
		assign(std::move(bytes));
		return *this;
	}

	const binary &bytes() const { return mBytes; }
	size_t size() const { return mBytes.size(); }
	bool empty() const { return mBytes.empty(); }

private:
	// vector's own assignment either reuses the buffer or frees it, and neither clears what was
	// there, so the wipe has to happen before it runs.
	template <typename T> void assign(T &&bytes) {
		Cleanse(mBytes);
		mBytes = std::forward<T>(bytes);
	}

	binary mBytes;
};

// Clears a buffer when the scope exits, for key material held in something whose type cannot be
// changed -- the public SFrameSendKey and SFrameReceiveKey carry a plain binary. Declare it after
// what it refers to, so it runs first.
class ScopedCleanse {
public:
	explicit ScopedCleanse(binary &buffer) : mBuffer(buffer) {}
	~ScopedCleanse() { Cleanse(mBuffer); }

	ScopedCleanse(const ScopedCleanse &) = delete;
	ScopedCleanse &operator=(const ScopedCleanse &) = delete;

private:
	binary &mBuffer;
};

// Throws std::invalid_argument if the key is shorter than MinBaseKeySize().
RTC_CPP_EXPORT void ValidateBaseKey(const binary &baseKey);

// Derive a per-SSRC base key from the shared base key.
// Wraps sframe_crypto::DeriveSSRCKey without exposing internal crypto headers.
RTC_CPP_EXPORT binary DeriveSSRCKey(SSRC ssrc, const binary &baseKey, uint16_t cipherSuiteId);

// Validate a descriptor byte against the packet's position in its object. S only on the
// first, E only on the last (a single-packet object is both), and T zero, since a
// packetized-origin object is declined rather than mis-delivered. The 5 reserved bits are
// ignored rather than required to be zero, so a future extension that sets one stays decodable.
RTC_CPP_EXPORT bool IsValidDescriptor(uint8_t descriptor, bool isFirst, bool isLast);

// True when the descriptor declares a packetized payload origin (T=1). Since both carry
// S | E, this is all that separates a per-packet object from a single-packet frame.
RTC_CPP_EXPORT bool IsPacketizedOrigin(uint8_t descriptor);

// Validate an incoming RTP packet and locate its payload. On success sets hdrSize to the
// RTP header size (payload starts at data[hdrSize]) and payloadEnd to the payload end
// with RTP padding removed. False for packets that must be dropped. Every read is bounds
// checked first: the declared extension length is itself inside the packet.
RTC_CPP_EXPORT bool ParseRtpPayload(const binary &packet, size_t &hdrSize, size_t &payloadEnd);

// As above, additionally requiring the 1-byte S/E/T descriptor at data[hdrSize] and at
// least one payload byte after it.
RTC_CPP_EXPORT bool ParseSFramePacket(const shared_ptr<Message> &msg, size_t &hdrSize,
                                      size_t &payloadEnd);

// True for well-formed RTP carrying no payload once padding is removed (RFC 3550 section
// 5.1). It belongs to no SFrame object and is not malformed, but it does consume a
// sequence number. The codec depacketizers skip such a packet the same way.
RTC_CPP_EXPORT bool HasNoRtpPayload(const binary &packet);

// The key and salt for one KID: builds both RFC 9605 Section 4.4.2 labels and runs the HKDF chain.
// Depends on (base key, KID, cipher suite) and not on the frame counter, so it runs once per KID
// rather than per frame. Wraps sframe_crypto without exposing internal crypto headers.
RTC_CPP_EXPORT SFrameKidKeys DeriveKeysForKid(const binary &baseKey, uint64_t kid,
                                              uint16_t cipherSuiteId);

// Ratchet a key forward by one step (RFC 9605 Section 5.1).
// Wraps sframe_crypto::RatchetKey without exposing internal crypto headers.
RTC_CPP_EXPORT binary RatchetKey(uint16_t cipherSuiteId, const binary &key);

// Decrypt all non-Control messages in place, dropping those that fail to authenticate
// and preserving frameInfo. The decoder comes from the set, which keeps one per SSRC and
// evicts only the idlest once full -- see SFrameDecoderSet.
// @param ssrc RTP SSRC the batch arrived on, for the per-SSRC key derivation.
RTC_CPP_EXPORT void DecryptMessages(message_vector &messages, SSRC ssrc,
                                    SFrameDecoderSet &decoders);

// Each frame decrypted under its own SSRC, for a batch spanning more than one. `owners` is
// parallel to `messages`.
RTC_CPP_EXPORT void DecryptMessages(message_vector &messages, const std::vector<SSRC> &owners,
                                    SFrameDecoderSet &decoders);

} // namespace sframe

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA

#endif // RTC_IMPL_SFRAME_UTILITY_H
