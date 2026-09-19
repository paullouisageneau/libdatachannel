/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_SFRAME_UTILITY_H
#define RTC_IMPL_SFRAME_UTILITY_H

#include "rtc/common.hpp"
#include "rtc/message.hpp"
#include "rtc/sframe.hpp"

#include <cstdint>
#include <memory>

namespace rtc::impl {

struct SFrameKeyMaterial {
	binary contentEncryptionKey;
	binary nonce;
};


/// Decoded SFrame header fields. length is the encoded header size in bytes.
struct SFrameHeaderInfo {
	uint64_t kid;
	uint64_t ctr;
	size_t length;
};

/// SFrame header, RFC 9605 Section 4.2
struct RTC_CPP_EXPORT SFrameHeader {
	// utility methods to encode/ decode headers
	static binary Encode(SFrameHeaderInfo headerInfo);
	static SFrameHeaderInfo Decode(const binary& encodedFrame);
};


/// SFrameUtility
struct RTC_CPP_EXPORT SFrameUtility {
	static SFrameKeyMaterial GetSFrameKeys(const uint64_t kid, const uint64_t ctr, const binary baseKey, const uint16_t cipherSuite);
	static binary MakeAAD(const binary& header, const binary& metadata);
	static binary GetSFrameKeyLabel(uint64_t kid, uint16_t cipherSuite);
	static binary GetSFrameSaltLabel(uint64_t kid, uint16_t cipherSuite);
	static binary DeriveNonce(const binary& sframeSalt, const uint64_t ctr, size_t nonceSize);

	// pt -> ct
	static binary EncodePlaintext(const uint16_t cipherSuite,
	                                            const binary& sframeKey,
	                                            const binary& nonce,
	                                            const binary& aad,
	                                            const binary& plaintext);

	// ct -> pt
	static binary DecodeCiphertext(const uint16_t cipherSuite,
	                                             const binary& sframeKey,
	                                             const binary& nonce,
	                                             const binary& aad,
	                                             const binary& cipherText);

	// helpers
	static size_t MinimalByteLength(uint64_t v);
	static binary EncodeBigEndian(uint64_t value, size_t numBytes);
	static uint64_t DecodeBigEndian(const binary& buf, size_t offset, size_t len);

	// Largest accepted ratchetStepBits. The KID is 64 bits and the key generation takes
	// whatever the step does not, so the step is capped well short of the field.
	static constexpr uint8_t MaxRatchetStepBits = 32;

	// KID = (keyGeneration << R) + (ratchetStep % (1 << R)), per RFC 9605 Section 5.1.
	static uint64_t MakeKid(uint64_t keyGeneration, uint64_t ratchetStep, uint8_t ratchetStepBits);

	// The two halves of a KID. A receiver needs these to tell a ratchet step, which it can
	// follow by ratcheting forward, from a new key generation, which it cannot.
	static uint64_t KeyGenerationFromKid(uint64_t kid, uint8_t ratchetStepBits);
	static uint64_t RatchetStepFromKid(uint64_t kid, uint8_t ratchetStepBits);

	// The KID one ratchet step on, wrapping the step to zero and leaving the key
	// generation untouched.
	static uint64_t NextRatchetKid(uint64_t kid, uint8_t ratchetStepBits);

	// Minimum base key size in bytes. RFC 9605 sets no length requirement, but below 128
	// bits no registered suite reaches its nominal security level.
	static size_t MinBaseKeySize();

	// Throws std::invalid_argument if the key is shorter than MinBaseKeySize().
	static void ValidateBaseKey(const binary& baseKey);

	// Derive a per-SSRC base key from the shared base key.
	// Wraps SFrameCrypto::DeriveSSRCKey without exposing internal crypto headers.
	static binary DeriveSSRCKey(uint32_t ssrc, const binary& baseKey, uint16_t cipherSuiteId);

	// Validate a descriptor byte against the packet's position in its object. S only on the
	// first, E only on the last (a single-packet object is both), T and reserved bits zero.
	static bool IsValidDescriptor(uint8_t descriptor, bool isFirst, bool isLast);

	// True when the descriptor declares a packetized payload origin (T=1). Since both carry
	// S | E, this is all that separates a per-packet object from a single-packet frame.
	static bool IsPacketizedOrigin(uint8_t descriptor);

	// Validate an incoming RTP packet and locate its payload. On success sets hdrSize to the
	// RTP header size (payload starts at data[hdrSize]) and payloadEnd to the payload end
	// with RTP padding removed. False for packets that must be dropped. Every read is bounds
	// checked first: the declared extension length is itself inside the packet.
	static bool ParseRtpPayload(const binary& packet, size_t& hdrSize, size_t& payloadEnd);

	// As above, additionally requiring the 1-byte S/E/T descriptor at data[hdrSize] and at
	// least one payload byte after it.
	static bool ParseSFramePacket(const std::shared_ptr<Message>& msg, size_t& hdrSize,
	                              size_t& payloadEnd);

	// Ratchet a key forward by one step (RFC 9605 Section 5.1).
	// Wraps SFrameCrypto::RatchetKey without exposing internal crypto headers.
	static binary RatchetKey(uint16_t cipherSuiteId, const binary& key);

	// Decrypt all non-Control messages in place, dropping those that fail to authenticate
	// and preserving frameInfo. Lazily creates the decoder, replacing it if the SSRC changes.
	// @param ssrc RTP SSRC the batch arrived on, for the per-SSRC key derivation.
	static void DecryptMessages(message_vector &messages,
	                            const std::shared_ptr<SFrameKeyProvider> &keyProvider,
	                            uint32_t ssrc, std::unique_ptr<SFrameDecoder> &decoder);
};


} // namespace rtc::impl

#endif // RTC_IMPL_SFRAME_UTILITY_H
