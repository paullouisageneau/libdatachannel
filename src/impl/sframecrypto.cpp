/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "sframecrypto.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#if USE_GNUTLS

#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>

#elif USE_MBEDTLS

#include "mbedtls/build_info.h"

#if MBEDTLS_VERSION_MAJOR >= 4

// Mbed TLS 4 removed the legacy low-level crypto modules (cipher, gcm, hkdf, md)
// from the public API: they now live in TF-PSA-Crypto and are only reachable
// through the PSA Crypto API.
#include "psa/crypto.h"

#include "tls.hpp" // rtc::mbedtls::safe_psa

#include <atomic>
#include <mutex>

#else

#include "mbedtls/cipher.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

#endif

#else // OpenSSL

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>

#endif


namespace {

// The backends take unsigned char buffers. std::byte has the same size and object
// representation and may alias any object, so this is a view, not a conversion.
inline const unsigned char *uc(const rtc::binary &b) {
	return reinterpret_cast<const unsigned char *>(b.data());
}
inline unsigned char *uc(rtc::binary &b) { return reinterpret_cast<unsigned char *>(b.data()); }

} // namespace

#if USE_GNUTLS


namespace rtc::gnutls {

size_t hashLength(gnutls_mac_algorithm_t hash) {
	return gnutls_hmac_get_len(hash);
}

binary hmac(const binary& key, gnutls_mac_algorithm_t hash, const binary& data) {
	binary mac(gnutls_hmac_get_len(hash));
	int ret = gnutls_hmac_fast(hash, key.data(), key.size(), data.data(), data.size(), mac.data());
	if (ret < 0)
		throw std::runtime_error(std::string("HMAC failed: ") + gnutls_strerror(ret));

	return mac;
}

// GnuTLS exposes no AES-CTR, so it is synthesized from AES-CBC: a single-block CBC
// encryption gives C0 = AES_ECB(P0 XOR IV), so IV=counter over a zero block yields
// AES_ECB(counter). XORing that keystream with the input gives the CTR output, with a
// 16-byte counter incremented as a 128-bit big-endian integer as OpenSSL does.
// `cipher` is the AES-CBC variant.
binary ctrXcrypt(const binary& key, gnutls_cipher_algorithm_t cipher, const binary& nonce, const binary& input) {
	binary output(input.size());
	if (input.empty())
		return output;

	gnutls_datum_t keyDatum;
	keyDatum.data = const_cast<unsigned char *>(uc(key));
	keyDatum.size = (unsigned int)key.size();

	// 16-byte counter, initialized to the zero-padded nonce.
	binary counter(nonce);
	counter.resize(16, std::byte{0});

	const uint8_t zeroBlock[16] = {0};
	uint8_t keystream[16];

	// The context is created once and the IV reset per block: creating it per block would
	// recompute the AES key schedule for every 16 bytes of payload.
	gnutls_datum_t ivDatum;
	ivDatum.data = uc(counter);
	ivDatum.size = 16;

	gnutls_cipher_hd_t handle;
	int ret = gnutls_cipher_init(&handle, cipher, &keyDatum, &ivDatum);
	if (ret < 0)
		throw std::runtime_error(std::string("AES-CTR init failed: ") + gnutls_strerror(ret));

	std::unique_ptr<std::remove_pointer_t<gnutls_cipher_hd_t>, decltype(&gnutls_cipher_deinit)>
	    ctx(handle, gnutls_cipher_deinit);

	size_t offset = 0;
	while (offset < input.size()) {
		gnutls_cipher_set_iv(ctx.get(), counter.data(), counter.size());

		ret = gnutls_cipher_encrypt2(ctx.get(), zeroBlock, 16, keystream, 16);
		if (ret < 0)
			throw std::runtime_error(std::string("AES-CTR encrypt failed: ") +
			                         gnutls_strerror(ret));

		size_t blockLen = std::min<size_t>(16, input.size() - offset);
		for (size_t i = 0; i < blockLen; ++i)
			output[offset + i] = input[offset + i] ^ std::byte(keystream[i]);

		offset += blockLen;

		// Increment the 128-bit big-endian counter.
		for (int i = 15; i >= 0; --i) {
			if (counter[i] = std::byte(std::to_integer<uint8_t>(counter[i]) + 1); counter[i] != std::byte{0})
				break;
		}
	}

	return output;
}

binary gcmEncrypt(const binary& key, gnutls_cipher_algorithm_t cipher, const binary& nonce, const binary& aad, const binary& plaintext, size_t tagLength) {
	gnutls_datum_t keyDatum;
	keyDatum.data = const_cast<unsigned char *>(uc(key));
	keyDatum.size = (unsigned int)key.size();

	gnutls_aead_cipher_hd_t handle;
	int ret = gnutls_aead_cipher_init(&handle, cipher, &keyDatum);
	if (ret < 0)
		throw std::runtime_error(std::string("AES-GCM init failed: ") + gnutls_strerror(ret));

	std::unique_ptr<std::remove_pointer_t<gnutls_aead_cipher_hd_t>,
	                decltype(&gnutls_aead_cipher_deinit)>
	    ctx(handle, gnutls_aead_cipher_deinit);

	// Output buffer holds ciphertext followed by the appended tag.
	binary out(plaintext.size() + tagLength);
	size_t outLen = out.size();

	ret = gnutls_aead_cipher_encrypt(ctx.get(), nonce.data(), nonce.size(),
	                                 aad.data(), aad.size(), tagLength,
	                                 plaintext.data(), plaintext.size(), out.data(), &outLen);
	if (ret < 0)
		throw std::runtime_error(std::string("AES-GCM encrypt failed: ") + gnutls_strerror(ret));

	out.resize(outLen);
	return out;
}

binary gcmDecrypt(const binary& key, gnutls_cipher_algorithm_t cipher, const binary& nonce, const binary& aad, const binary& ciphertextAndTag, size_t tagLength) {
	if (ciphertextAndTag.size() < tagLength)
		throw std::invalid_argument("Payload shorter than tag length");

	gnutls_datum_t keyDatum;
	keyDatum.data = const_cast<unsigned char *>(uc(key));
	keyDatum.size = (unsigned int)key.size();

	gnutls_aead_cipher_hd_t handle;
	int ret = gnutls_aead_cipher_init(&handle, cipher, &keyDatum);
	if (ret < 0)
		throw std::runtime_error(std::string("AES-GCM init failed: ") + gnutls_strerror(ret));

	std::unique_ptr<std::remove_pointer_t<gnutls_aead_cipher_hd_t>,
	                decltype(&gnutls_aead_cipher_deinit)>
	    ctx(handle, gnutls_aead_cipher_deinit);

	binary plaintext(ciphertextAndTag.size() - tagLength);
	size_t plaintextLen = plaintext.size();

	// GnuTLS expects the ciphertext with the tag appended (which is our input layout).
	ret = gnutls_aead_cipher_decrypt(ctx.get(), nonce.data(), nonce.size(),
	                                 aad.data(), aad.size(), tagLength,
	                                 ciphertextAndTag.data(), ciphertextAndTag.size(),
	                                 plaintext.data(), &plaintextLen);
	if (ret < 0) {
		if (ret == GNUTLS_E_DECRYPTION_FAILED)
			throw std::runtime_error("AES-GCM authentication failed");
		throw std::runtime_error(std::string("AES-GCM decrypt failed: ") + gnutls_strerror(ret));
	}

	plaintext.resize(plaintextLen);
	return plaintext;
}

// HKDF-Extract with no salt: per RFC 5869 the salt defaults to a string of Nh
// zero bytes.
binary hkdfExtract(const binary& ikm, gnutls_mac_algorithm_t hash) {
	const size_t hashLen = gnutls_hmac_get_len(hash);
	binary prk(hashLen);
	binary zeroSalt(hashLen, std::byte{0});

	gnutls_datum_t keyDatum;
	keyDatum.data = const_cast<unsigned char *>(uc(ikm));
	keyDatum.size = (unsigned int)ikm.size();

	gnutls_datum_t saltDatum;
	saltDatum.data = uc(zeroSalt);
	saltDatum.size = (unsigned int)zeroSalt.size();

	int ret = gnutls_hkdf_extract(hash, &keyDatum, &saltDatum, prk.data());
	if (ret < 0)
		throw std::runtime_error(std::string("HKDF extract failed: ") + gnutls_strerror(ret));

	return prk;
}

binary hkdfExtract(const binary& ikm, const binary& salt, gnutls_mac_algorithm_t hash) {
	const size_t hashLen = gnutls_hmac_get_len(hash);
	binary prk(hashLen);

	gnutls_datum_t keyDatum;
	keyDatum.data = const_cast<unsigned char *>(uc(ikm));
	keyDatum.size = (unsigned int)ikm.size();

	gnutls_datum_t saltDatum;
	saltDatum.data = const_cast<unsigned char *>(uc(salt));
	saltDatum.size = (unsigned int)salt.size();

	int ret = gnutls_hkdf_extract(hash, &keyDatum, &saltDatum, prk.data());
	if (ret < 0)
		throw std::runtime_error(std::string("HKDF extract failed: ") + gnutls_strerror(ret));

	return prk;
}

binary hkdfExpand(const binary& prk, gnutls_mac_algorithm_t hash, const binary& info, size_t length) {
	binary key(length);

	gnutls_datum_t prkDatum;
	prkDatum.data = const_cast<unsigned char *>(uc(prk));
	prkDatum.size = (unsigned int)prk.size();

	gnutls_datum_t infoDatum;
	infoDatum.data = const_cast<unsigned char *>(uc(info));
	infoDatum.size = (unsigned int)info.size();

	int ret = gnutls_hkdf_expand(hash, &prkDatum, &infoDatum, key.data(), length);
	if (ret < 0)
		throw std::runtime_error(std::string("HKDF expand failed: ") + gnutls_strerror(ret));

	return key;
}

} // namespace rtc::gnutls

namespace rtc::impl {

namespace backend = ::rtc::gnutls;
}

#elif USE_MBEDTLS && MBEDTLS_VERSION_MAJOR >= 4

namespace rtc::mbedtls {

namespace {

// SFrameCrypto is reachable without ever creating a peer connection, so this backend
// initializes PSA itself rather than relying on rtc::impl::Init(). psa_crypto_init() is
// idempotent and the success flag only avoids repeating it; a failure is deliberately not
// cached, or one transient error would disable SFrame for the process lifetime.
void ensureInit() {
	static std::atomic<bool> initialized{false};
	if (initialized.load(std::memory_order_acquire))
		return;

	// Serialised through the same mutex the rest of the codebase uses for PSA calls
	// when Mbed TLS is built without threading support.
	psa_status_t status =
	    static_cast<psa_status_t>(rtc::mbedtls::safe_psa([] { return psa_crypto_init(); }));
	if (status != PSA_SUCCESS)
		throw std::runtime_error("psa_crypto_init failed");

	initialized.store(true, std::memory_order_release);
}

// PSA takes keys by identifier rather than by buffer, so every operation imports
// its raw key into a volatile key slot.
class Key final {
public:
	Key(psa_key_type_t type, psa_algorithm_t algorithm, psa_key_usage_t usage,
	    const binary& key) {
		psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
		psa_set_key_type(&attributes, type);
		psa_set_key_algorithm(&attributes, algorithm);
		psa_set_key_usage_flags(&attributes, usage);

		psa_status_t status = psa_import_key(&attributes, uc(key), key.size(), &mId);
		psa_reset_key_attributes(&attributes);
		if (status != PSA_SUCCESS)
			throw std::runtime_error("Failed to import key");
	}

	~Key() { psa_destroy_key(mId); }

	Key(const Key&) = delete;
	Key& operator=(const Key&) = delete;

	mbedtls_svc_key_id_t id() const { return mId; }

private:
	mbedtls_svc_key_id_t mId = MBEDTLS_SVC_KEY_ID_INIT;
};

// Owns a cipher operation so it is aborted on every return and throw path.
struct CipherOperation final {
	psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;

	~CipherOperation() { psa_cipher_abort(&operation); }
};

// Owns a key derivation operation so it is aborted on every return and throw path.
struct DerivationOperation final {
	psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;

	~DerivationOperation() { psa_key_derivation_abort(&operation); }
};

void derivationInput(psa_key_derivation_operation_t *operation, psa_key_derivation_step_t step, const binary& data) {
	// An empty label is a legal HKDF info string, and vector::data() may return
	// null when empty, so always hand PSA a dereferenceable pointer.
	static const uint8_t empty = 0;
	const uint8_t *ptr = !data.empty() ? uc(data) : &empty;
	if (psa_key_derivation_input_bytes(operation, step, ptr, data.size()) != PSA_SUCCESS)
		throw std::runtime_error("HKDF input failed");
}

} // namespace

size_t hashLength(psa_algorithm_t hash) {
	const size_t length = PSA_HASH_LENGTH(hash);
	if (length == 0)
		throw std::runtime_error("Unsupported hash type in SFrameCrypto");

	return length;
}

binary hmac(const binary& key, psa_algorithm_t hash, const binary& data) {
	ensureInit();

	const psa_algorithm_t algorithm = PSA_ALG_HMAC(hash);
	Key macKey(PSA_KEY_TYPE_HMAC, algorithm, PSA_KEY_USAGE_SIGN_MESSAGE, key);

	binary mac(hashLength(hash));
	size_t macLength = 0;
	if (psa_mac_compute(macKey.id(), algorithm, uc(data), data.size(), uc(mac), mac.size(),
	                    &macLength) != PSA_SUCCESS)
		throw std::runtime_error("HMAC failed");

	mac.resize(macLength);
	return mac;
}

binary ctrXcrypt(const binary& key, psa_algorithm_t cipher, const binary& nonce, const binary& input) {
	binary output(input.size());
	if (input.empty())
		return output;

	ensureInit();

	// CTR mode uses the encrypt direction for both encryption and decryption.
	Key cipherKey(PSA_KEY_TYPE_AES, cipher, PSA_KEY_USAGE_ENCRYPT, key);

	CipherOperation ctx;
	if (psa_cipher_encrypt_setup(&ctx.operation, cipherKey.id(), cipher) != PSA_SUCCESS)
		throw std::runtime_error("AES-CTR setup failed");

	// AES-CTR requires a full 16-byte counter block; zero-pad the nonce if shorter.
	binary iv(nonce);
	iv.resize(16, std::byte{0});

	if (psa_cipher_set_iv(&ctx.operation, uc(iv), iv.size()) != PSA_SUCCESS)
		throw std::runtime_error("AES-CTR set iv failed");

	size_t outLen = 0, total = 0;
	if (psa_cipher_update(&ctx.operation, uc(input), input.size(), uc(output), output.size(),
	                      &outLen) != PSA_SUCCESS)
		throw std::runtime_error("AES-CTR update failed");

	total = outLen;

	if (psa_cipher_finish(&ctx.operation, uc(output) + total, output.size() - total, &outLen) != PSA_SUCCESS)
		throw std::runtime_error("AES-CTR final failed");

	total += outLen;
	output.resize(total);

	return output;
}

binary gcmEncrypt(const binary& key, psa_algorithm_t cipher, const binary& nonce, const binary& aad, const binary& plaintext, size_t tagLength) {
	ensureInit();

	const psa_algorithm_t algorithm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(cipher, tagLength);
	Key aeadKey(PSA_KEY_TYPE_AES, algorithm, PSA_KEY_USAGE_ENCRYPT, key);

	// PSA appends the tag to the ciphertext, which is the layout SFrame wants.
	binary out(plaintext.size() + tagLength);
	size_t outLen = 0;
	if (psa_aead_encrypt(aeadKey.id(), algorithm, uc(nonce), nonce.size(), uc(aad), aad.size(),
	                     uc(plaintext), plaintext.size(), uc(out), out.size(),
	                     &outLen) != PSA_SUCCESS)
		throw std::runtime_error("AES-GCM encrypt failed");

	out.resize(outLen);
	return out;
}

binary gcmDecrypt(const binary& key, psa_algorithm_t cipher, const binary& nonce, const binary& aad, const binary& ciphertextAndTag, size_t tagLength) {
	if (ciphertextAndTag.size() < tagLength)
		throw std::invalid_argument("Payload shorter than tag length");

	ensureInit();

	const psa_algorithm_t algorithm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(cipher, tagLength);
	Key aeadKey(PSA_KEY_TYPE_AES, algorithm, PSA_KEY_USAGE_DECRYPT, key);

	binary plaintext(ciphertextAndTag.size() - tagLength);
	size_t ptLen = 0;

	// psa_aead_decrypt() returns PSA_ERROR_INVALID_SIGNATURE on tag mismatch
	psa_status_t status = psa_aead_decrypt(aeadKey.id(), algorithm, uc(nonce), nonce.size(),
	                                      uc(aad), aad.size(), uc(ciphertextAndTag),
	                                      ciphertextAndTag.size(), uc(plaintext),
	                                      plaintext.size(), &ptLen);
	if (status == PSA_ERROR_INVALID_SIGNATURE)
		throw std::runtime_error("AES-GCM authentication failed");
	else if (status != PSA_SUCCESS)
		throw std::runtime_error("AES-GCM decrypt failed");

	plaintext.resize(ptLen);
	return plaintext;
}

binary hkdfExtract(const binary& ikm, const binary& salt, psa_algorithm_t hash) {
	ensureInit();

	binary prk(hashLength(hash));

	DerivationOperation ctx;
	if (psa_key_derivation_setup(&ctx.operation, PSA_ALG_HKDF_EXTRACT(hash)) != PSA_SUCCESS)
		throw std::runtime_error("HKDF extract setup failed");

	// HKDF-Extract takes the salt then the input keying material, in that order.
	derivationInput(&ctx.operation, PSA_KEY_DERIVATION_INPUT_SALT, salt);
	derivationInput(&ctx.operation, PSA_KEY_DERIVATION_INPUT_SECRET, ikm);

	if (psa_key_derivation_output_bytes(&ctx.operation, uc(prk), prk.size()) != PSA_SUCCESS)
		throw std::runtime_error("HKDF extract failed");

	return prk;
}

// HKDF-Extract with no salt
binary hkdfExtract(const binary& ikm, psa_algorithm_t hash) {
	// RFC 5869: an absent salt is equivalent to a hash-length string of zeros.
	return hkdfExtract(ikm, binary(hashLength(hash), std::byte{0}), hash);
}

binary hkdfExpand(const binary& prk, psa_algorithm_t hash, const binary& info, size_t length) {
	ensureInit();

	binary key(length);

	DerivationOperation ctx;
	if (psa_key_derivation_setup(&ctx.operation, PSA_ALG_HKDF_EXPAND(hash)) != PSA_SUCCESS)
		throw std::runtime_error("HKDF expand setup failed");

	// HKDF-Expand takes the pseudorandom key then the info string, in that order.
	derivationInput(&ctx.operation, PSA_KEY_DERIVATION_INPUT_SECRET, prk);
	derivationInput(&ctx.operation, PSA_KEY_DERIVATION_INPUT_INFO, info);

	if (psa_key_derivation_output_bytes(&ctx.operation, uc(key), key.size()) != PSA_SUCCESS)
		throw std::runtime_error("derive failed");

	return key;
}

} // namespace rtc::mbedtls

namespace rtc::impl {
namespace backend = ::rtc::mbedtls;
}

#elif USE_MBEDTLS

namespace rtc::mbedtls {

size_t hashLength(mbedtls_md_type_t hash) {
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(hash);
	if (!info)
		throw std::runtime_error("Unsupported hash type in SFrameCrypto");

	return mbedtls_md_get_size(info);
}

binary hmac(const binary& key, mbedtls_md_type_t hash, const binary& data) {
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(hash);
	if (!info)
		throw std::runtime_error("Unsupported hash type for HMAC");

	binary mac(mbedtls_md_get_size(info));
	if (mbedtls_md_hmac(info, uc(key), key.size(), uc(data), data.size(), uc(mac)) != 0)
		throw std::runtime_error("HMAC failed");

	return mac;
}

binary ctrXcrypt(const binary& key, mbedtls_cipher_type_t cipher, const binary& nonce, const binary& input) {
	binary output(input.size());

	const mbedtls_cipher_info_t *cipherInfo = mbedtls_cipher_info_from_type(cipher);
	if (!cipherInfo)
		throw std::runtime_error("Failed to look up AES-CTR cipher info");

	std::unique_ptr<mbedtls_cipher_context_t, void (*)(mbedtls_cipher_context_t *)> ctx(
	    new mbedtls_cipher_context_t, [](mbedtls_cipher_context_t *c) {
		    mbedtls_cipher_free(c);
		    delete c;
	    });
	mbedtls_cipher_init(ctx.get());

	if (mbedtls_cipher_setup(ctx.get(), cipherInfo) != 0)
		throw std::runtime_error("AES-CTR setup failed");

	// CTR mode uses the encrypt direction for both encryption and decryption.
	if (mbedtls_cipher_setkey(ctx.get(), uc(key), (int)(key.size() * 8), MBEDTLS_ENCRYPT) != 0)
		throw std::runtime_error("AES-CTR set key failed");

	// AES-CTR requires a full 16-byte IV; zero-pad the nonce if shorter.
	binary iv(nonce);
	iv.resize(16, std::byte{0});

	if (mbedtls_cipher_set_iv(ctx.get(), uc(iv), iv.size()) != 0)
		throw std::runtime_error("AES-CTR set iv failed");

	if (mbedtls_cipher_reset(ctx.get()) != 0)
		throw std::runtime_error("AES-CTR reset failed");

	size_t outLen = 0, total = 0;
	if (!input.empty() &&
	    mbedtls_cipher_update(ctx.get(), uc(input), input.size(), uc(output), &outLen) != 0)
		throw std::runtime_error("AES-CTR update failed");

	total = outLen;

	if (mbedtls_cipher_finish(ctx.get(), uc(output) + total, &outLen) != 0)
		throw std::runtime_error("AES-CTR final failed");

	total += outLen;
	output.resize(total);

	return output;
}

binary gcmEncrypt(const binary& key, mbedtls_cipher_type_t cipher, const binary& nonce, const binary& aad, const binary& plaintext, size_t tagLength) {
	(void)cipher; // Mbed TLS GCM is keyed by AES id + key length directly.

	std::unique_ptr<mbedtls_gcm_context, void (*)(mbedtls_gcm_context *)> ctx(
	    new mbedtls_gcm_context, [](mbedtls_gcm_context *c) {
		    mbedtls_gcm_free(c);
		    delete c;
	    });
	mbedtls_gcm_init(ctx.get());

	if (mbedtls_gcm_setkey(ctx.get(), MBEDTLS_CIPHER_ID_AES, uc(key), (unsigned int)(key.size() * 8)) != 0)
		throw std::runtime_error("AES-GCM set key failed");

	binary ct(plaintext.size());
	binary tag(tagLength);

	if (mbedtls_gcm_crypt_and_tag(ctx.get(), MBEDTLS_GCM_ENCRYPT, plaintext.size(),
	                              uc(nonce), nonce.size(), uc(aad), aad.size(),
	                              uc(plaintext), uc(ct), tag.size(), uc(tag)) != 0)
		throw std::runtime_error("AES-GCM encrypt failed");

	binary out = ct;
	out.insert(out.end(), tag.begin(), tag.end());
	return out;
}

binary gcmDecrypt(const binary& key, mbedtls_cipher_type_t cipher, const binary& nonce, const binary& aad, const binary& ciphertextAndTag, size_t tagLength) {
	(void)cipher; // Mbed TLS GCM is keyed by AES id + key length directly.

	if (ciphertextAndTag.size() < tagLength)
		throw std::invalid_argument("Payload shorter than tag length");

	size_t ctLen = ciphertextAndTag.size() - tagLength;
	const unsigned char* ctPtr  = uc(ciphertextAndTag);
	const unsigned char* tagPtr = uc(ciphertextAndTag) + ctLen;

	std::unique_ptr<mbedtls_gcm_context, void (*)(mbedtls_gcm_context *)> ctx(
	    new mbedtls_gcm_context, [](mbedtls_gcm_context *c) {
		    mbedtls_gcm_free(c);
		    delete c;
	    });
	mbedtls_gcm_init(ctx.get());

	if (mbedtls_gcm_setkey(ctx.get(), MBEDTLS_CIPHER_ID_AES, uc(key), (unsigned int)(key.size() * 8)) != 0)
		throw std::runtime_error("AES-GCM set key failed");

	binary plaintext(ctLen);

	// mbedtls_gcm_auth_decrypt returns MBEDTLS_ERR_GCM_AUTH_FAILED on tag mismatch
	int ret = mbedtls_gcm_auth_decrypt(ctx.get(), ctLen, uc(nonce), nonce.size(),
	                                   uc(aad), aad.size(), tagPtr, tagLength,
	                                   ctPtr, uc(plaintext));
	if (ret == MBEDTLS_ERR_GCM_AUTH_FAILED)
		throw std::runtime_error("AES-GCM authentication failed");
	else if (ret != 0)
		throw std::runtime_error("AES-GCM decrypt failed");

	return plaintext;
}

// HKDF-Extract with no salt
binary hkdfExtract(const binary& ikm, mbedtls_md_type_t hash) {
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(hash);
	if (!info)
		throw std::runtime_error("Unsupported hash type for HKDF");

	const size_t hashLen = mbedtls_md_get_size(info);
	binary prk(hashLen);
	binary salt(hashLen, std::byte{0});

	if (mbedtls_hkdf_extract(info, uc(salt), salt.size(), uc(ikm), ikm.size(), uc(prk)) != 0)
		throw std::runtime_error("HKDF extract failed");

	return prk;
}

binary hkdfExtract(const binary& ikm, const binary& salt, mbedtls_md_type_t hash) {
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(hash);
	if (!info)
		throw std::runtime_error("Unsupported hash type for HKDF");

	const size_t hashLen = mbedtls_md_get_size(info);
	binary prk(hashLen);

	if (mbedtls_hkdf_extract(info, uc(salt), salt.size(), uc(ikm), ikm.size(), uc(prk)) != 0)
		throw std::runtime_error("HKDF extract failed");

	return prk;
}

binary hkdfExpand(const binary& prk, mbedtls_md_type_t hash, const binary& info, size_t length) {
	const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(hash);
	if (!mdInfo)
		throw std::runtime_error("Unsupported hash type for HKDF");

	binary key(length);

	if (mbedtls_hkdf_expand(mdInfo, uc(prk), prk.size(), uc(info), info.size(), uc(key), length) != 0)
		throw std::runtime_error("derive failed");

	return key;
}

} // namespace rtc::mbedtls

namespace rtc::impl {
namespace backend = ::rtc::mbedtls;
}

#else // OpenSSL

namespace rtc::openssl {

size_t hashLength(const EVP_MD* hash) {
	return EVP_MD_size(hash);
}

binary hmac(const binary& key, const EVP_MD* hash, const binary& data) {
	unsigned int macLen = 0;
	uint8_t fullTag[EVP_MAX_MD_SIZE];
	if (!HMAC(hash, uc(key), (int)key.size(), uc(data), data.size(), fullTag, &macLen))
		throw std::runtime_error("HMAC failed");

	return binary(reinterpret_cast<const std::byte *>(fullTag),
	              reinterpret_cast<const std::byte *>(fullTag) + macLen);
}

binary ctrXcrypt(const binary& key, const EVP_CIPHER* cipher, const binary& nonce, const binary& input) {
	binary output(input.size());

	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);

	if (!ctx)
		throw std::runtime_error("Failed to alloc CTR ctx");

	// OpenSSL AES-CTR requires a full 16-byte IV; zero-pad the nonce if shorter.
	int ivLen = EVP_CIPHER_iv_length(cipher);
	binary iv(nonce);
	iv.resize(static_cast<size_t>(ivLen), std::byte{0});

	if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, uc(key), uc(iv)) != 1)
		throw std::runtime_error("AES-CTR init failed");

	int len = 0, clen = 0;
	if (!input.empty() && (EVP_EncryptUpdate(ctx.get(), uc(output), &len, uc(input), (int)input.size()) != 1))
		throw std::runtime_error("AES-CTR update failed");

	clen = len;

	if (EVP_EncryptFinal_ex(ctx.get(), uc(output) + clen, &len) != 1)
		throw std::runtime_error("AES-CTR final failed");

	clen += len;
	output.resize(clen);

	return output;
}

binary gcmEncrypt(const binary& key, const EVP_CIPHER* cipher, const binary& nonce, const binary& aad, const binary& plaintext, size_t tagLength) {
	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);

	if (!ctx)
		throw std::runtime_error("Failed to alloc AES ctx");

	if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, uc(key), uc(nonce)) != 1)
		throw std::runtime_error("AES-GCM init failed");

	int len = 0;
	if (EVP_EncryptUpdate(ctx.get(), nullptr, &len, uc(aad), (int)aad.size()) != 1)
		throw std::runtime_error("AES-GCM AAD update failed");

	binary ct(plaintext.size());
	if (EVP_EncryptUpdate(ctx.get(), uc(ct), &len, uc(plaintext), (int)plaintext.size()) != 1)
		throw std::runtime_error("AES-GCM encrypt failed");

	int ciphertextLen = len;
	if (EVP_EncryptFinal_ex(ctx.get(), uc(ct) + len, &len) != 1)
		throw std::runtime_error("AES-GCM final failed");

	ciphertextLen += len;
	ct.resize(ciphertextLen);

	binary tag(tagLength);
	if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, (int)tag.size(), tag.data()) != 1)
		throw std::runtime_error("AES-GCM get tag failed");

	binary out = ct;
	out.insert(out.end(), tag.begin(), tag.end());
	return out;
}

binary gcmDecrypt(const binary& key, const EVP_CIPHER* cipher, const binary& nonce, const binary& aad, const binary& ciphertextAndTag, size_t tagLength) {
	if (ciphertextAndTag.size() < tagLength)
		throw std::invalid_argument("Payload shorter than tag length");

	size_t ctLen = ciphertextAndTag.size() - tagLength;
	const unsigned char* ctPtr  = uc(ciphertextAndTag);
	const unsigned char* tagPtr = uc(ciphertextAndTag) + ctLen;

	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);

	if (!ctx)
		throw std::runtime_error("Failed to alloc AES ctx");

	if (EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, uc(key), uc(nonce)) != 1)
		throw std::runtime_error("AES-GCM init failed");

	int len = 0;
	if (EVP_DecryptUpdate(ctx.get(), nullptr, &len, uc(aad), (int)aad.size()) != 1)
		throw std::runtime_error("AES-GCM AAD update failed");

	binary plaintext(ctLen);
	int plaintextLen = 0;
	if (ctLen > 0) {
		if (EVP_DecryptUpdate(ctx.get(), uc(plaintext), &len, ctPtr, (int)ctLen) != 1)
			throw std::runtime_error("AES-GCM decrypt failed");

		// Only meaningful when the payload update ran: the AAD update above also writes
		// len, so inheriting it here would report aad.size() fabricated plaintext bytes
		// for a legitimately empty frame.
		plaintextLen = len;
	}

	if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, (int)tagLength, const_cast<uint8_t*>(tagPtr)) != 1)
		throw std::runtime_error("AES-GCM set tag failed");

	// Decrypt Final: returns 0 if tag does not match. GCM is a stream cipher mode and
	// emits no final block, so collect it in scratch space rather than writing past the
	// end of plaintext, which is empty for an empty frame.
	uint8_t finalBlock[EVP_MAX_BLOCK_LENGTH];
	auto match = EVP_DecryptFinal_ex(ctx.get(), finalBlock, &len);
	if (!match)
		throw std::runtime_error("AES-GCM authentication failed");

	plaintext.resize(plaintextLen);

	return plaintext;
}

binary hkdfExtract(const binary& ikm, const EVP_MD* hash) {
	const size_t hashLen = EVP_MD_size(hash);
	binary prk(hashLen);

	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL), EVP_PKEY_CTX_free);

	if (!pctx)
		throw std::runtime_error("Failed to create HKDF context");

	if (EVP_PKEY_derive_init(pctx.get()) <= 0)
		throw std::runtime_error("HKDF derive init failed");

	if (EVP_PKEY_CTX_set_hkdf_mode(pctx.get(), EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) <= 0)
		throw std::runtime_error("HKDF set extract-only mode failed");

	if (EVP_PKEY_CTX_set_hkdf_md(pctx.get(), hash) <= 0)
		throw std::runtime_error("HKDF set hash function failed");

	if (EVP_PKEY_CTX_set1_hkdf_key(pctx.get(), uc(ikm), (int)ikm.size()) <= 0)
		throw std::runtime_error("HKDF set key failed");

	size_t outLen = prk.size();
	if (EVP_PKEY_derive(pctx.get(), uc(prk), &outLen) <= 0)
		throw std::runtime_error("HKDF extract failed");

	if (outLen != hashLen)
		throw std::runtime_error("HKDF extract output size mismatch");

	return prk;
}

binary hkdfExtract(const binary& ikm, const binary& salt, const EVP_MD* hash) {
	const size_t hashLen = EVP_MD_size(hash);
	binary prk(hashLen);

	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL), EVP_PKEY_CTX_free);

	if (!pctx)
		throw std::runtime_error("Failed to create HKDF context");

	if (EVP_PKEY_derive_init(pctx.get()) <= 0)
		throw std::runtime_error("HKDF derive init failed");

	if (EVP_PKEY_CTX_set_hkdf_mode(pctx.get(), EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) <= 0)
		throw std::runtime_error("HKDF set extract-only mode failed");

	if (EVP_PKEY_CTX_set_hkdf_md(pctx.get(), hash) <= 0)
		throw std::runtime_error("HKDF set hash function failed");

	if (EVP_PKEY_CTX_set1_hkdf_salt(pctx.get(), uc(salt), (int)salt.size()) <= 0)
		throw std::runtime_error("HKDF set salt failed");

	if (EVP_PKEY_CTX_set1_hkdf_key(pctx.get(), uc(ikm), (int)ikm.size()) <= 0)
		throw std::runtime_error("HKDF set key failed");

	size_t outLen = prk.size();
	if (EVP_PKEY_derive(pctx.get(), uc(prk), &outLen) <= 0)
		throw std::runtime_error("HKDF extract failed");

	if (outLen != hashLen)
		throw std::runtime_error("HKDF extract output size mismatch");

	return prk;
}

binary hkdfExpand(const binary& prk, const EVP_MD* hash, const binary& info, size_t length) {
	binary key(length);

	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL), EVP_PKEY_CTX_free);

	if (!pctx)
		throw std::runtime_error("Failed to create HKDF context");

	if (EVP_PKEY_derive_init(pctx.get()) <= 0)
		throw std::runtime_error("HKDF derive init failed");

	if (EVP_PKEY_CTX_set_hkdf_mode(pctx.get(), EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) <= 0)
		throw std::runtime_error("HKDF set expand-only mode failed");

	if (EVP_PKEY_CTX_set_hkdf_md(pctx.get(), hash) <= 0)
		throw std::runtime_error("HKDF set hash function failed");

	if (EVP_PKEY_CTX_add1_hkdf_info(pctx.get(), uc(info), (int)info.size()) <= 0)
		throw std::runtime_error("HKDF add info function failed");

	if (EVP_PKEY_CTX_set1_hkdf_key(pctx.get(), uc(prk), (int)prk.size()) <= 0)
		throw std::runtime_error("HKDF set key failed");

	size_t outLength = length;
	if (EVP_PKEY_derive(pctx.get(), uc(key), &outLength) <= 0)
		 throw std::runtime_error("derive failed");

	return key;
}

} // namespace rtc::openssl

namespace rtc::impl {
namespace backend = ::rtc::openssl;
}

#endif

// Backend-neutral SFrame implementation: RFC 9605 byte manipulation only, driving the
// backend primitives through `backend`.

namespace rtc::impl {

// Backend-neutral aliases for the cipher and hash identifiers used by the active
// crypto backend.
#if USE_GNUTLS
using SFrameCipherType = gnutls_cipher_algorithm_t;
using SFrameHashType = gnutls_mac_algorithm_t;
#elif USE_MBEDTLS && MBEDTLS_VERSION_MAJOR >= 4
// Mbed TLS 4 identifies both ciphers and hashes with psa_algorithm_t. The AES key
// length is carried by the key material itself rather than by the cipher id.
using SFrameCipherType = psa_algorithm_t;
using SFrameHashType = psa_algorithm_t;
#elif USE_MBEDTLS
using SFrameCipherType = mbedtls_cipher_type_t;
using SFrameHashType = mbedtls_md_type_t;
#else // OpenSSL
using SFrameCipherType = const EVP_CIPHER*;
using SFrameHashType = const EVP_MD*;
#endif

struct SFrameCipherSuiteInfo {
	uint16_t cipherSuiteId;
	SFrameCipherType cipher;
	SFrameHashType hkdfHash;
	size_t combinedKeySize;
	size_t encryptionKeySize;
	size_t nonceSize;
	size_t tagLength;
};

struct SFrameSubKeys {
	binary encryptionKey;
	binary authenticationKey;
};

static SFrameCipherSuiteInfo GetCipherSuiteInfo(const uint16_t cipherSuiteId);
static SFrameSubKeys SplitSFrameKey(const size_t combinedKeySize, const size_t encryptionKeySize, const binary& sframeKey);
static bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t n);
static binary ComputeTag(const SFrameSubKeys keys, SFrameHashType hash, const binary& nonce, const binary& aad, const binary& ct, size_t tagLength);
static bool VerifyTag(const SFrameSubKeys keys, SFrameHashType hash, const binary& nonce, const binary& aad, const binary& ciphertext, const binary& tag, size_t tagLength);
static binary TruncateTag(const binary& tag, size_t n);
static binary EncodePlaintextForCTRHMAC(const SFrameSubKeys keys, SFrameCipherSuiteInfo cipherSuiteInfo, const binary& nonce, const binary& aad, const binary& plaintext);
static binary DecodeCiphertextForCTRHMAC(const SFrameSubKeys keys, SFrameCipherSuiteInfo cipherSuiteInfo, const binary& nonce, const binary& aad, const binary& ciphertextAndTag);
static binary deriveSFrameSecret(const binary& baseKey, SFrameHashType suiteHash);
static binary deriveSFrameSecret(const binary& baseKey, const binary& salt, SFrameHashType suiteHash);
static binary deriveSFrameKey(const binary& sframeSecret, SFrameHashType suiteHash, const binary& sFrameKeyLabel, size_t keySize);
static binary deriveSFrameSalt(const binary& sframeSecret, SFrameHashType suiteHash, const binary& sFrameSaltLabel, size_t nonceSize);

// Constant-time comparison of two equal-length buffers. Returns true if equal.
bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t n) {
	uint8_t diff = 0;
	for (size_t i = 0; i < n; ++i)
		diff |= (uint8_t)(a[i] ^ b[i]);

	return diff == 0;
}

// Map each cipher suite's logical AEAD/cipher and hash to the active backend's identifier.
// These are the only backend-specific values in the suite table below; the key, nonce and
// tag lengths are shared. Under GnuTLS the CTR suites map to the AES-CBC variant, from
// which gnutls::ctrXcrypt() synthesizes the CTR keystream.
#if USE_GNUTLS
#define SFRAME_CIPHER_AES_128_CTR GNUTLS_CIPHER_AES_128_CBC
#define SFRAME_CIPHER_AES_256_CTR GNUTLS_CIPHER_AES_256_CBC
#define SFRAME_CIPHER_AES_128_GCM GNUTLS_CIPHER_AES_128_GCM
#define SFRAME_CIPHER_AES_256_GCM GNUTLS_CIPHER_AES_256_GCM
#define SFRAME_HASH_SHA_256 GNUTLS_MAC_SHA256
#define SFRAME_HASH_SHA_512 GNUTLS_MAC_SHA512
#elif USE_MBEDTLS && MBEDTLS_VERSION_MAJOR >= 4
// PSA derives the AES variant from the key length, so the 128- and 256-bit suites
// share a cipher algorithm.
#define SFRAME_CIPHER_AES_128_CTR PSA_ALG_CTR
#define SFRAME_CIPHER_AES_256_CTR PSA_ALG_CTR
#define SFRAME_CIPHER_AES_128_GCM PSA_ALG_GCM
#define SFRAME_CIPHER_AES_256_GCM PSA_ALG_GCM
#define SFRAME_HASH_SHA_256 PSA_ALG_SHA_256
#define SFRAME_HASH_SHA_512 PSA_ALG_SHA_512
#elif USE_MBEDTLS
#define SFRAME_CIPHER_AES_128_CTR MBEDTLS_CIPHER_AES_128_CTR
#define SFRAME_CIPHER_AES_256_CTR MBEDTLS_CIPHER_AES_256_CTR
#define SFRAME_CIPHER_AES_128_GCM MBEDTLS_CIPHER_AES_128_GCM
#define SFRAME_CIPHER_AES_256_GCM MBEDTLS_CIPHER_AES_256_GCM
#define SFRAME_HASH_SHA_256 MBEDTLS_MD_SHA256
#define SFRAME_HASH_SHA_512 MBEDTLS_MD_SHA512
#else // OpenSSL
#define SFRAME_CIPHER_AES_128_CTR EVP_aes_128_ctr()
#define SFRAME_CIPHER_AES_256_CTR EVP_aes_256_ctr()
#define SFRAME_CIPHER_AES_128_GCM EVP_aes_128_gcm()
#define SFRAME_CIPHER_AES_256_GCM EVP_aes_256_gcm()
#define SFRAME_HASH_SHA_256 EVP_sha256()
#define SFRAME_HASH_SHA_512 EVP_sha512()
#endif

// The IETF defined SFrame ciphersuites as specified in RFC9605 and draft-barnes-sframe-iana-256
SFrameCipherSuiteInfo GetCipherSuiteInfo(const uint16_t cipherSuiteId) {
	switch (cipherSuiteId) {
		case 0x01:
			return { cipherSuiteId, SFRAME_CIPHER_AES_128_CTR, SFRAME_HASH_SHA_256, 48, 16, 12, 10 };
		case 0x02:
			return { cipherSuiteId, SFRAME_CIPHER_AES_128_CTR, SFRAME_HASH_SHA_256, 48, 16, 12, 8 };
		case 0x03:
			return { cipherSuiteId, SFRAME_CIPHER_AES_128_CTR, SFRAME_HASH_SHA_256, 48, 16, 12, 4 };
		case 0x04:
			return { cipherSuiteId, SFRAME_CIPHER_AES_128_GCM, SFRAME_HASH_SHA_256, 16, 16, 12, 16 };
		case 0x05:
			return { cipherSuiteId, SFRAME_CIPHER_AES_256_GCM, SFRAME_HASH_SHA_512, 32, 32, 12, 16 };
		case 0x06:
			return { cipherSuiteId, SFRAME_CIPHER_AES_256_CTR, SFRAME_HASH_SHA_512, 96, 32, 12, 10 };
		case 0x07:
			return { cipherSuiteId, SFRAME_CIPHER_AES_256_CTR, SFRAME_HASH_SHA_512, 96, 32, 12, 8 };
		case 0x08:
			return { cipherSuiteId, SFRAME_CIPHER_AES_256_CTR, SFRAME_HASH_SHA_512, 96, 32, 12, 4 };
		default:
			throw std::runtime_error("Unsupported SFrame Cipher Suite ID in GetCipherSuiteInfo: " + std::to_string(cipherSuiteId));
	}
}

#undef SFRAME_CIPHER_AES_128_CTR
#undef SFRAME_CIPHER_AES_256_CTR
#undef SFRAME_CIPHER_AES_128_GCM
#undef SFRAME_CIPHER_AES_256_GCM
#undef SFRAME_HASH_SHA_256
#undef SFRAME_HASH_SHA_512

size_t SFrameCrypto::GetNonceSize(uint16_t cipherSuiteId) {
	return GetCipherSuiteInfo(cipherSuiteId).nonceSize;
}

binary SFrameCrypto::RatchetKey(const uint16_t cipherSuiteId, const binary& baseKey) {
	// RFC 9605 Section 5.1:
	// next_key = HKDF-Expand(HKDF-Extract(salt=zeros, ikm=current_key), "SFrame 1.0 Ratchet", Nh)
	// Output length Nh equals the hash output size, preserving key length across ratchet steps.
	SFrameCipherSuiteInfo suiteInfo = GetCipherSuiteInfo(cipherSuiteId);
	const size_t nh = backend::hashLength(suiteInfo.hkdfHash);

	// HKDF-Extract(salt=zeros/default, ikm=current_key)
	binary prk = deriveSFrameSecret(baseKey, suiteInfo.hkdfHash);

	// HKDF-Expand(prk, "SFrame 1.0 Ratchet", Nh)
	static const std::string kRatchetInfo = "SFrame 1.0 Ratchet";
	binary info(reinterpret_cast<const std::byte *>(kRatchetInfo.data()),
	            reinterpret_cast<const std::byte *>(kRatchetInfo.data()) + kRatchetInfo.size());

	return backend::hkdfExpand(prk, suiteInfo.hkdfHash, info, nh);
}

binary SFrameCrypto::EncodePlaintext(const uint16_t cipherSuiteId,
                                                   const binary& sframeKey,
                                                   const binary& nonce,
                                                   const binary& aad,
                                                   const binary& plaintext) {
	SFrameCipherSuiteInfo cipherSuiteInfo = GetCipherSuiteInfo(cipherSuiteId);

	SFrameSubKeys subKeys = SplitSFrameKey(cipherSuiteInfo.combinedKeySize, cipherSuiteInfo.encryptionKeySize, sframeKey);

	switch (cipherSuiteId) {
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x06:
		case 0x07:
		case 0x08:
			// For Suite 1-3 and 6-8, use the CTR + HMAC mode
			return EncodePlaintextForCTRHMAC(subKeys, cipherSuiteInfo, nonce, aad, plaintext);
		case 0x04:
		case 0x05:
			// For Suite 4-5, use AES-GCM mode
			return backend::gcmEncrypt(subKeys.encryptionKey, cipherSuiteInfo.cipher, nonce, aad, plaintext, cipherSuiteInfo.tagLength);
		default:
			throw std::runtime_error("Unsupported SFrame Cipher Suite ID in SFrameCrypto::EncodePlaintext: " + std::to_string(cipherSuiteId));
	}
}

binary SFrameCrypto::DecodeCiphertext(const uint16_t cipherSuiteId,
                                                    const binary& sframeKey,
                                                    const binary& nonce,
                                                    const binary& aad,
                                                    const binary& ciphertextAndTag) {
	SFrameCipherSuiteInfo cipherSuiteInfo = GetCipherSuiteInfo(cipherSuiteId);

	SFrameSubKeys subKeys = SplitSFrameKey(cipherSuiteInfo.combinedKeySize, cipherSuiteInfo.encryptionKeySize, sframeKey);

	switch (cipherSuiteId) {
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x06:
		case 0x07:
		case 0x08:
			// For Suite 1-3 and 6-8, use the CTR + HMAC mode
			return DecodeCiphertextForCTRHMAC(subKeys, cipherSuiteInfo, nonce, aad, ciphertextAndTag);
		case 0x04:
		case 0x05:
			// For Suite 4-5, use AES-GCM mode
			return backend::gcmDecrypt(subKeys.encryptionKey, cipherSuiteInfo.cipher, nonce, aad, ciphertextAndTag, cipherSuiteInfo.tagLength);
		default:
			throw std::runtime_error("Unsupported SFrame Cipher Suite ID in SFrameCrypto::DecodeCiphertext: " + std::to_string(cipherSuiteId));
	}
}

binary EncodePlaintextForCTRHMAC(const SFrameSubKeys keys,
                                               SFrameCipherSuiteInfo cipherSuiteInfo,
                                               const binary& nonce,
                                               const binary& aad,
                                               const binary& plaintext) {
	auto ct = backend::ctrXcrypt(keys.encryptionKey, cipherSuiteInfo.cipher, nonce, plaintext);

	auto tag = ComputeTag(keys, cipherSuiteInfo.hkdfHash, nonce, aad, ct, cipherSuiteInfo.tagLength);

	binary out;
	out.reserve(ct.size() + tag.size());
	out.insert(out.end(), ct.begin(), ct.end());
	out.insert(out.end(), tag.begin(), tag.end());

	return out;
}

binary DecodeCiphertextForCTRHMAC(const SFrameSubKeys keys,
                                                SFrameCipherSuiteInfo cipherSuiteInfo,
                                                const binary& nonce,
                                                const binary& aad,
                                                const binary& ciphertextAndTag) {
	if (ciphertextAndTag.size() < cipherSuiteInfo.tagLength) {
		throw std::invalid_argument("Payload shorter than tag length");
	}

	size_t ctLen = ciphertextAndTag.size() - cipherSuiteInfo.tagLength;

	binary ciphertext(ciphertextAndTag.begin(), ciphertextAndTag.begin() + ctLen );
	binary tag(ciphertextAndTag.begin() + ctLen , ciphertextAndTag.end());

	if (!VerifyTag(keys, cipherSuiteInfo.hkdfHash, nonce, aad, ciphertext, tag, cipherSuiteInfo.tagLength)) {
		throw std::runtime_error("Authentication failed");
	}

	// AES-CTR decryption is just AES-CTR encryption
	auto pt = backend::ctrXcrypt(keys.encryptionKey, cipherSuiteInfo.cipher, nonce, ciphertext);

	return pt;
}

binary ComputeTag(const SFrameSubKeys keys,
                                SFrameHashType hash,
                                const binary& nonce,
                                const binary& aad,
                                const binary& ct, size_t tagLength) {
	// Helper to append a 64 bit int in big endian
	auto append_u64_be = [&](binary& buf, uint64_t v) {
		for (int i = 7; i >= 0; --i) {
			buf.push_back(std::byte((v >> (8*i)) & 0xFF));
		}
	};

	// Build auth_data
	// From RFC 9605 4.5.1 auth_data = aad_len + ct_len + tag_len + nonce + aad + ct
	binary authData;
	authData.reserve(8*3 + nonce.size() + aad.size() + ct.size());

	// add the lengths aad_len + ct_len + tag_len
	append_u64_be(authData, aad.size());
	append_u64_be(authData, ct.size());
	append_u64_be(authData, tagLength);

	// now append nonce + aad + ct
	authData.insert(authData.end(), nonce.begin(), nonce.end());
	authData.insert(authData.end(), aad.begin(), aad.end());
	authData.insert(authData.end(), ct.begin(), ct.end());

	// Compute the full-length HMAC, then truncate to tagLength bytes
	binary fullTag = backend::hmac(keys.authenticationKey, hash, authData);
	return TruncateTag(fullTag, tagLength);
}

bool VerifyTag(const SFrameSubKeys keys,
               SFrameHashType hash,
               const binary& nonce,
               const binary& aad,
               const binary& ciphertext,
               const binary& tag, size_t tagLength) {
	if (tag.size() != tagLength)
		return false;

	// recompute the tag and verify we have what we expect (constant-time comparison)
	auto expected = ComputeTag(keys, hash, nonce, aad, ciphertext, tagLength);
	return expected.size() == tag.size() && constantTimeEqual(uc(expected), uc(tag), tag.size());
}

binary TruncateTag(const binary& tag, size_t n) {
	if (tag.size() < n)
		throw std::invalid_argument("Tag length is shorter than requested truncation length");

	return binary(tag.begin(), tag.begin() + n);
}

SFrameSubKeys SplitSFrameKey(const size_t combinedKeySize, const size_t encryptionKeySize, const binary& sframeKey) {
	// Reachable with a caller-supplied key through the exported EncodePlaintext and
	// DecodeCiphertext, so the key must be measured before it is split.
	if (sframeKey.size() < combinedKeySize)
		throw std::invalid_argument("SFrame key is shorter than the cipher suite requires");

	if (sframeKey.size() > combinedKeySize)
		throw std::invalid_argument("SFrame key is longer than the cipher suite requires");

	if (encryptionKeySize == combinedKeySize) {
		return { sframeKey, {} };
	} else if (encryptionKeySize < combinedKeySize) {
		return {
			binary(sframeKey.begin(), sframeKey.begin() + encryptionKeySize),
			binary(sframeKey.begin() + encryptionKeySize, sframeKey.end())
		};
	}

	throw std::invalid_argument("Unable to split sframe key, size was too big");
}

void SFrameCrypto::ValidateBaseKey(const binary& baseKey) {
	if (baseKey.size() < MinBaseKeySize)
		throw std::invalid_argument("SFrame base key must be at least " +
		                            std::to_string(MinBaseKeySize) + " bytes, got " +
		                            std::to_string(baseKey.size()));
}

// Derive the material needed to encode sframes using various HKDF functions
SFrameDerivedKeys SFrameCrypto::DeriveSFrameKeys(SFrameKeyInfo keyInfo) {
	// Checked here as well as at the API boundary: this is the one point every send and
	// receive path funnels through, so a key provider that hands back an empty base key
	// for an unknown KID cannot produce a key an attacker could derive too.
	ValidateBaseKey(keyInfo.baseKey);

	SFrameCipherSuiteInfo cipherSuiteInfo = GetCipherSuiteInfo(keyInfo.cipherSuiteId);

	binary sframeSecret = deriveSFrameSecret(keyInfo.baseKey, cipherSuiteInfo.hkdfHash);

	binary sframeKey = deriveSFrameKey(sframeSecret, cipherSuiteInfo.hkdfHash, keyInfo.keyLabel, cipherSuiteInfo.combinedKeySize);

	binary sframeSalt = deriveSFrameSalt(sframeSecret, cipherSuiteInfo.hkdfHash, keyInfo.saltLabel, cipherSuiteInfo.nonceSize);

	SFrameDerivedKeys keyMaterial = {sframeKey, sframeSalt};

	return keyMaterial;
}

// Per-SSRC key derivation per draft-ietf-avtcore-rtp-sframe Section 7:
// ssrc_key = HKDF-Expand(HKDF-Extract(SSRC, base_key), "SFrame 1.0 RTP Stream", CipherSuite.Nh)
binary SFrameCrypto::DeriveSSRCKey(uint32_t ssrc, const binary& baseKey, uint16_t cipherSuiteId) {
	ValidateBaseKey(baseKey);

	SFrameCipherSuiteInfo suiteInfo = GetCipherSuiteInfo(cipherSuiteId);
	const size_t nh = backend::hashLength(suiteInfo.hkdfHash);

	// Encode SSRC as 4-byte big-endian salt
	binary ssrcSalt(4);
	ssrcSalt[0] = std::byte((ssrc >> 24) & 0xFF);
	ssrcSalt[1] = std::byte((ssrc >> 16) & 0xFF);
	ssrcSalt[2] = std::byte((ssrc >>  8) & 0xFF);
	ssrcSalt[3] = std::byte( ssrc        & 0xFF);

	// HKDF-Extract(salt=SSRC, ikm=base_key)
	binary prk = deriveSFrameSecret(baseKey, ssrcSalt, suiteInfo.hkdfHash);

	// HKDF-Expand(prk, info="SFrame 1.0 RTP Stream", L=Nh)
	static const std::string kInfo = "SFrame 1.0 RTP Stream";
	binary info(reinterpret_cast<const std::byte *>(kInfo.data()),
	            reinterpret_cast<const std::byte *>(kInfo.data()) + kInfo.size());

	return backend::hkdfExpand(prk, suiteInfo.hkdfHash, info, nh);
}

// Derive sframe secret using HKDF-Extract - RFC 9605 Section 4.4.2.
binary deriveSFrameSecret(const binary& baseKey, SFrameHashType suiteHash) {
	return backend::hkdfExtract(baseKey, suiteHash);
}

// Derive sframe secret using HKDF-Extract with an explicit salt value.
binary deriveSFrameSecret(const binary& baseKey, const binary& salt, SFrameHashType suiteHash) {
	return backend::hkdfExtract(baseKey, salt, suiteHash);
}

// Derive sframe key using HKDF-Expand - RFC 9605 Section 4.4.2.
binary deriveSFrameKey(const binary& sframeSecret,
                                     SFrameHashType suiteHash,
                                     const binary& sFrameKeyLabel,
                                     size_t keySize) {
	return backend::hkdfExpand(sframeSecret, suiteHash, sFrameKeyLabel, keySize);
}

// Derive sframe salt using HKDF-Expand - RFC 9605 Section 4.4.2.
binary deriveSFrameSalt(const binary& sframeSecret,
                                      SFrameHashType suiteHash,
                                      const binary& sFrameSaltLabel,
                                      size_t nonceSize) {
	return backend::hkdfExpand(sframeSecret, suiteHash, sFrameSaltLabel, nonceSize);
}

} // namespace rtc::impl
