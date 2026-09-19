/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_CRYPTO_H
#define RTC_SFRAME_CRYPTO_H

#include "common.hpp"

#include <cstdint>
#include <vector>

namespace rtc::impl {

struct SFrameKeyInfo {
	binary baseKey;
	const uint16_t cipherSuiteId;
	binary keyLabel;
	binary saltLabel;
};

struct SFrameDerivedKeys {
	binary contentEncryptionKey;
	binary sframeSalt;
};

/// Backend-neutral SFrame cryptography (RFC 9605).
struct RTC_CPP_EXPORT SFrameCrypto {

	// RFC 9605 places no length requirement on base_key — it is HKDF input keying
	// material — but an empty or short key is derivable by anyone who can guess it, and
	// every derived key inherits that weakness. 16 bytes is the floor: below 128 bits no
	// registered cipher suite can reach its nominal security level.
	static constexpr size_t MinBaseKeySize = 16;

	// Rejects a base key that cannot safely seed the KDF.
	// Throws std::invalid_argument if shorter than MinBaseKeySize.
	static void ValidateBaseKey(const binary& baseKey);

	static SFrameDerivedKeys DeriveSFrameKeys(SFrameKeyInfo keyInfo);
	static binary EncodePlaintext(const uint16_t cipherSuiteId, const binary& sframeKey, const binary& nonce, const binary& aad, const binary& plaintext);

	static binary DecodeCiphertext(const uint16_t cipherSuiteId, const binary& sframeKey, const binary& nonce, const binary& aad, const binary& ciphertextAndTag);

	static binary RatchetKey(const uint16_t cipherSuiteId, const binary& baseKey);

	// Per-SSRC key derivation per draft-ietf-avtcore-rtp-sframe Section 7:
	// ssrc_key = HKDF-Expand(HKDF-Extract(SSRC, base_key), "SFrame 1.0 RTP Stream", CipherSuite.Nh)
	// SSRC is encoded as a 4-byte big-endian salt value.
	static binary DeriveSSRCKey(uint32_t ssrc, const binary& baseKey, uint16_t cipherSuiteId);

	// Returns the nonce size for a given cipher suite ID.
	static size_t GetNonceSize(uint16_t cipherSuiteId);
};

} // namespace rtc::impl

#endif // RTC_SFRAME_CRYPTO_H
