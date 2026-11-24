/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_SFRAME_CRYPTO_H
#define RTC_IMPL_SFRAME_CRYPTO_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "rtp.hpp"

#include <cstdint>

namespace rtc::impl {

struct SFrameDerivedKeys {
	binary contentEncryptionKey;
	binary sframeSalt;
};

/// Backend-neutral SFrame cryptography (RFC 9605).
// Not exported: unlike sframe and sframe::header, nothing outside the library calls these
// directly, so they stay internal like the rest of src/impl.
namespace sframe_crypto {

// RFC 9605 places no length requirement on base_key -- it is HKDF input keying material -- but
// below 128 bits no registered cipher suite reaches its nominal security level, and every derived
// key inherits that weakness.
const size_t MinBaseKeySize = 16;

// Rejects a base key that cannot safely seed the KDF.
// Throws std::invalid_argument if shorter than MinBaseKeySize.
void ValidateBaseKey(const binary &baseKey);

// Rejects a nonce whose length is not the suite's. The backends disagree on a wrong one.
// Throws std::invalid_argument.
void ValidateNonce(size_t nonceSize, const binary &nonce);

// Throws std::invalid_argument if the suite is not in the SFrame registry. 0 is what an
// uninitialised field carries, so it is rejected like any other unregistered value.
void ValidateCipherSuite(uint16_t cipherSuiteId);

// Compares two buffers of key material without branching on their contents. Lengths are
// compared first and in the clear, which leaks nothing: a cipher suite's key length is
// public.
bool ConstantTimeEquals(const binary &a, const binary &b);

// Overwrites a buffer of key material with zeroes, so it does not outlive its use in freed heap.
// Dispatches to the crypto backend's own primitive. Leaves the buffer's length alone: the bytes
// are cleared, not released.
void Cleanse(binary &buffer);

// Takes the base key by reference: the caller owns it and clears it on its own terms, and a copy
// taken here would be one more block of master key to account for on every KID derived.
SFrameDerivedKeys DeriveSFrameKeys(const binary &baseKey, uint16_t cipherSuiteId,
                                   const binary &keyLabel, const binary &saltLabel);
binary EncodePlaintext(uint16_t cipherSuiteId, const binary &sframeKey, const binary &nonce,
                       const binary &aad, const binary &plaintext);

binary DecodeCiphertext(uint16_t cipherSuiteId, const binary &sframeKey, const binary &nonce,
                        const binary &aad, const binary &ciphertextAndTag);

binary RatchetKey(uint16_t cipherSuiteId, const binary &baseKey);

// Per-SSRC key derivation per draft-ietf-avtcore-rtp-sframe Section 7:
// ssrc_key = HKDF-Expand(HKDF-Extract(SSRC, base_key), "SFrame 1.0 RTP Stream", CipherSuite.Nh)
// SSRC is encoded as a 4-byte big-endian salt value.
binary DeriveSSRCKey(SSRC ssrc, const binary &baseKey, uint16_t cipherSuiteId);

size_t GetNonceSize(uint16_t cipherSuiteId);

} // namespace sframe_crypto

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA

#endif // RTC_IMPL_SFRAME_CRYPTO_H
