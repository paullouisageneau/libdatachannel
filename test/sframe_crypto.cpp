/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframecodec.hpp"
#include "impl/sframeutility.hpp"
#include "rtc/rtc.hpp"
#include "test.hpp"
#include <iostream>
#include <set>
#include <string>
#include <thread>

#if RTC_ENABLE_MEDIA

using namespace rtc;

static binary convertFromHex(const string &hex);

struct HeaderTestData {
	uint64_t kid{};
	uint64_t ctr{};
	binary encoded;

	HeaderTestData &Kid(uint64_t v) {
		kid = v;
		return *this;
	}
	HeaderTestData &Ctr(uint64_t v) {
		ctr = v;
		return *this;
	}
	HeaderTestData &Encoded(const string &h) {
		encoded = convertFromHex(h);
		return *this;
	}
};

// Named setters, not positional init: every vector field below is binary, so two could be
// transposed silently. (Designated initializers are C++20.)
struct EncryptionTestData {
	uint8_t cipherSuite{};
	uint64_t kid{};
	uint64_t ctr{};
	binary baseKey;
	binary sFrameKeyLabel;
	binary sFrameSaltLabel;
	binary sFrameSecret;
	binary sFrameKey;
	binary sFrameSalt;
	binary nonce;
	binary metadata;
	binary aad;
	binary pt;
	binary ct;

	EncryptionTestData &CipherSuite(uint8_t v) {
		cipherSuite = v;
		return *this;
	}
	EncryptionTestData &Kid(uint64_t v) {
		kid = v;
		return *this;
	}
	EncryptionTestData &Ctr(uint64_t v) {
		ctr = v;
		return *this;
	}
	EncryptionTestData &BaseKey(const string &h) {
		baseKey = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &SFrameKeyLabel(const string &h) {
		sFrameKeyLabel = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &SFrameSaltLabel(const string &h) {
		sFrameSaltLabel = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &SFrameSecret(const string &h) {
		sFrameSecret = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &SFrameKey(const string &h) {
		sFrameKey = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &SFrameSalt(const string &h) {
		sFrameSalt = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &Nonce(const string &h) {
		nonce = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &Metadata(const string &h) {
		metadata = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &Aad(const string &h) {
		aad = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &Pt(const string &h) {
		pt = convertFromHex(h);
		return *this;
	}
	EncryptionTestData &Ct(const string &h) {
		ct = convertFromHex(h);
		return *this;
	}
};

struct CTRHMACTestData {
	uint8_t cipherSuite{};
	binary key;
	binary enc_key;
	binary auth_key;
	binary nonce;
	binary aad;
	binary pt;
	binary ct;

	CTRHMACTestData &CipherSuite(uint8_t v) {
		cipherSuite = v;
		return *this;
	}
	CTRHMACTestData &Key(const string &h) {
		key = convertFromHex(h);
		return *this;
	}
	CTRHMACTestData &EncKey(const string &h) {
		enc_key = convertFromHex(h);
		return *this;
	}
	CTRHMACTestData &AuthKey(const string &h) {
		auth_key = convertFromHex(h);
		return *this;
	}
	CTRHMACTestData &Nonce(const string &h) {
		nonce = convertFromHex(h);
		return *this;
	}
	CTRHMACTestData &Aad(const string &h) {
		aad = convertFromHex(h);
		return *this;
	}
	CTRHMACTestData &Pt(const string &h) {
		pt = convertFromHex(h);
		return *this;
	}
	CTRHMACTestData &Ct(const string &h) {
		ct = convertFromHex(h);
		return *this;
	}
};

// Answers for one key generation. The same material can be handed to an encoder and to this
// without the two roles colliding, because the key types differ.
class DecodeKeysProvider : public SFrameReceiveKeyProvider {
public:
	DecodeKeysProvider(uint16_t cipherSuiteId, binary baseKey, uint64_t kid = 0)
	    : SFrameReceiveKeyProvider(cipherSuiteId, /*ratchetStepBits=*/0,
	                               /*perSsrcDerivation=*/false) {
		addKey(kid, SFrameReceiveKey{std::move(baseKey)});
	}
};

// A provider that answers for an unknown KID with a placeholder instead of nullopt. An
// empty base key derives a key an attacker can derive too, so without the length check
// frames forged under any unknown KID would authenticate.
class EmptyKeyProvider : public SFrameReceiveKeyProvider {
public:
	explicit EmptyKeyProvider(uint16_t cipherSuiteId)
	    : SFrameReceiveKeyProvider(cipherSuiteId, 0, /*perSsrcDerivation=*/false) {}

	optional<SFrameReceiveKey> receiveKey(uint64_t) const override {
		return SFrameReceiveKey{binary()};
	}
};

static void test_sframe_headers();
static void test_sframe_encode_decode();
static void test_aes_ctr_hmac();
static void test_sframe_short_ciphertext();
static void test_sframe_authentication_failures();
static void test_sframe_empty_plaintext();
static void test_sframe_counter_uniqueness();
static void test_sframe_bounds_checks();
static void test_sframe_ratchet_kid_field();
static void test_sframe_key_validation();
static void test_sframe_key_cleansing();
static void test_sframe_cipher_suite_validation();
static void test_sframe_per_ssrc_key_derivation();
static binary convertFromHex(const string &hex);
static string convertToHex(const binary &bytes);
static std::vector<HeaderTestData> loadTestHeaderData();
static std::vector<EncryptionTestData> loadTestEncryptionData();
static std::vector<CTRHMACTestData> loadTestHMACCTRData();

bool compareEncoded(const binary &a, const binary &b);

TestResult test_sframe_crypto() {
	InitLogger(LogLevel::Debug);

	try {
		test_sframe_headers();
		test_sframe_encode_decode();
		test_aes_ctr_hmac();
		test_sframe_short_ciphertext();
		test_sframe_authentication_failures();
		test_sframe_empty_plaintext();
		test_sframe_counter_uniqueness();
		test_sframe_bounds_checks();
		test_sframe_ratchet_kid_field();
		test_sframe_key_validation();
		test_sframe_key_cleansing();
		test_sframe_cipher_suite_validation();
		test_sframe_per_ssrc_key_derivation();

		std::cout << "Success" << std::endl;
		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

void test_sframe_headers() {
	auto headerTestVectors = loadTestHeaderData();

	std::atomic<int> headerFailures = 0;
	for (size_t i = 0; i < headerTestVectors.size(); ++i) {
		const auto &headerTestData = headerTestVectors[i];

		// get our encoding
		impl::SFrameHeaderInfo headerInfo = {headerTestData.kid, headerTestData.ctr, 0};
		binary encoded = impl::sframe::header::Encode(headerInfo);

		// and verify that
		if (!compareEncoded(encoded, headerTestData.encoded)) {
			std::cout << "Encoded Header Test failed\n";
			std::cout << "  kid: " << headerTestData.kid << ", ctr: " << headerTestData.ctr << "\n";
			headerFailures++;
		}

		// now, test the decoding
		impl::SFrameHeaderInfo decodedHeaderInfo =
		    impl::sframe::header::Decode(headerTestData.encoded);
		if (decodedHeaderInfo.kid != headerTestData.kid) {
			std::cout << "Decoded Header Test incorrect kid \n";
			std::cout << "  kid: " << headerTestData.kid << "\n";
			headerFailures++;
		}

		if (decodedHeaderInfo.ctr != headerTestData.ctr) {
			std::cout << "Decoded Header Test incorrect ctr \n";
			std::cout << "  ctr: " << headerTestData.ctr << "\n";
			headerFailures++;
		}
	}

	try {
		if (headerFailures != 0) {
			throw std::runtime_error("Failed Encoding Header Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

void test_sframe_encode_decode() {
	auto encryptionTestVectors = loadTestEncryptionData();

	std::atomic<int> encryptionFailures = 0;
	for (size_t i = 0; i < encryptionTestVectors.size(); ++i) {
		const auto &encryptionTestData = encryptionTestVectors[i];

		auto sendProvider = std::make_shared<SFrameSendKeyProvider>(
		    encryptionTestData.cipherSuite, /*ratchetStepBits=*/0, /*ratchetPeriod=*/0,
		    /*perSsrcDerivation=*/false,
		    SFrameSendKey{encryptionTestData.baseKey, encryptionTestData.kid,
		                  encryptionTestData.ctr});

		auto checkIntermediate = [&](const char *what, const binary &got, const binary &expected) {
			if (compareEncoded(got, expected))
				return;
			std::cout << "EncryptionTest incorrect " << what << "\n";
			std::cout << "  Suite: " << int(encryptionTestData.cipherSuite)
			          << ", kid: " << encryptionTestData.kid << ", ctr: " << encryptionTestData.ctr
			          << "\n";
			std::cout << "  Expected: " << convertToHex(expected) << "\n";
			std::cout << "  Got:      " << convertToHex(got) << "\n";
			encryptionFailures++;
		};

		// The vectors pin every step of the derivation, not just the ciphertext. Checking
		// them individually names the step that broke instead of reporting a ciphertext
		// mismatch, and catches a derivation error that the round-trip below would miss
		// because encode and decode would agree on the same wrong key.
		//
		// sFrameSecret and sFrameSalt are not reachable through the public API, but they
		// are covered transitively: a wrong secret changes both the key and the nonce, and
		// a wrong salt changes the nonce.
		{
			checkIntermediate("sframe key label",
			                  impl::sframe::GetSFrameKeyLabel(encryptionTestData.kid,
			                                                  encryptionTestData.cipherSuite),
			                  encryptionTestData.sFrameKeyLabel);

			checkIntermediate("sframe salt label",
			                  impl::sframe::GetSFrameSaltLabel(encryptionTestData.kid,
			                                                   encryptionTestData.cipherSuite),
			                  encryptionTestData.sFrameSaltLabel);

			impl::SFrameKeyMaterial derived = impl::sframe::GetSFrameKeys(
			    encryptionTestData.kid, encryptionTestData.ctr, encryptionTestData.baseKey,
			    encryptionTestData.cipherSuite);
			checkIntermediate("sframe key", derived.contentEncryptionKey,
			                  encryptionTestData.sFrameKey);
			checkIntermediate("nonce", derived.nonce, encryptionTestData.nonce);

			impl::SFrameHeaderInfo headerInfo = {encryptionTestData.kid, encryptionTestData.ctr, 0};
			checkIntermediate("aad",
			                  impl::sframe::MakeAAD(impl::sframe::header::Encode(headerInfo),
			                                        encryptionTestData.metadata),
			                  encryptionTestData.aad);
		}

		// The RFC vectors pin a fixed KID, so ratcheting is off: a non-zero period here
		// would need a ratchet field and would move the KID away from the vector.
		impl::SFrameEncoder encoder(sendProvider);

		// store the pt in a Message struct
		message_ptr msg = make_message(binary(encryptionTestData.pt), Message::Binary);

		message_ptr encodedMsg = encoder.encodeFrame(msg, encryptionTestData.metadata);

		binary ct = binary((encodedMsg)->begin(), (encodedMsg)->end());

		if (!compareEncoded(ct, encryptionTestData.ct)) {
			std::cout << "EncryptionTest incorrect ct\n";
			std::cout << "Suite: " << int(encryptionTestData.cipherSuite)
			          << ", kid: " << encryptionTestData.kid << ", ctr: " << encryptionTestData.ctr
			          << "\n";
			encryptionFailures++;
		}

		// okay, let's test the decode
		// create a class to provide the decode keys
		auto decodeKeysProvider = std::make_shared<DecodeKeysProvider>(
		    encryptionTestData.cipherSuite, encryptionTestData.baseKey, encryptionTestData.kid);

		// create the decoder
		impl::SFrameDecoder decoder(decodeKeysProvider);

		// other key details are the same
		message_ptr ctMsg = make_message(binary(encryptionTestData.ct), Message::Binary);
		message_ptr decodedMsg = decoder.decodeFrame(ctMsg, encryptionTestData.metadata);
		binary pt = binary((decodedMsg)->begin(), (decodedMsg)->end());

		// and verify that
		if (!compareEncoded(pt, encryptionTestData.pt)) {
			std::cout << "DecryptionTest failed \n";
			std::cout << "Suite: " << int(encryptionTestData.cipherSuite)
			          << ", kid: " << encryptionTestData.kid << ", ctr: " << encryptionTestData.ctr
			          << "\n";
			encryptionFailures++;
		}
	}

	try {
		if (encryptionFailures != 0) {
			throw std::runtime_error("Failed Encoding Encryption Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

void test_aes_ctr_hmac() {
	auto encryptionTestVectors = loadTestHMACCTRData();

	std::atomic<int> encryptionFailures = 0;
	for (size_t i = 0; i < encryptionTestVectors.size(); ++i) {
		const auto &encryptionTestData = encryptionTestVectors[i];

		// get our sframe keys
		impl::SFrameKeyMaterial keys = {encryptionTestData.key, encryptionTestData.nonce};

		// finally encrypt the plaintext
		binary ct = impl::sframe::EncodePlaintext(encryptionTestData.cipherSuite,
		                                          keys.contentEncryptionKey, keys.nonce,
		                                          encryptionTestData.aad, encryptionTestData.pt);

		if (!compareEncoded(ct, encryptionTestData.ct)) {
			std::cout << "EncryptionTest incorrect ct\n";
			encryptionFailures++;
		}

		// okay, lets test the decode
		binary pt = impl::sframe::DecodeCiphertext(encryptionTestData.cipherSuite,
		                                           keys.contentEncryptionKey, keys.nonce,
		                                           encryptionTestData.aad, encryptionTestData.ct);

		if (!compareEncoded(pt, encryptionTestData.pt)) {
			std::cout << "DecryptionTest incorrect ct\n";
			encryptionFailures++;
		}
	}

	try {
		if (encryptionFailures != 0) {
			throw std::runtime_error("Failed AES CTR HMAC Encryption Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// A ciphertext shorter than the suite's authentication tag must be rejected with
// std::invalid_argument by every cipher suite and every crypto backend. The length
// is attacker-controlled (impl::SFrameDecoder::decodeSFrame slices the payload off after
// the SFrame header without checking it against the tag length), so a backend that
// computes `ciphertextAndTag.size() - tagLength` unguarded wraps the size_t around
// and derives an out-of-bounds tag pointer before failing.
void test_sframe_short_ciphertext() {
	struct SuiteInfo {
		uint8_t cipherSuite;
		size_t combinedKeySize; // SplitSFrameKey needs a key of exactly this size
		size_t tagLength;
	};

	// Per the IANA SFrame registry; mirrors GetCipherSuiteInfo().
	static const std::vector<SuiteInfo> suites = {
	    {0x01, 48, 10}, {0x02, 48, 8},  {0x03, 48, 4}, {0x04, 16, 16},
	    {0x05, 32, 16}, {0x06, 96, 10}, {0x07, 96, 8}, {0x08, 96, 4},
	};

	std::atomic<int> shortCiphertextFailures = 0;

	for (const auto &suite : suites) {
		const binary key(suite.combinedKeySize, std::byte{0x11});
		const binary nonce(12, std::byte{0x22});
		const binary aad(4, std::byte{0x33});

		// Every length below the tag length must be rejected, including empty.
		for (size_t len = 0; len < suite.tagLength; ++len) {
			const binary shortCt(len, std::byte{0x44});
			try {
				impl::sframe::DecodeCiphertext(suite.cipherSuite, key, nonce, aad, shortCt);
				std::cout << "ShortCiphertextTest accepted a short payload\n";
				std::cout << "  Suite: " << int(suite.cipherSuite) << ", payload: " << len
				          << ", tag length: " << suite.tagLength << "\n";
				shortCiphertextFailures++;
			} catch (const std::invalid_argument &) {
				// Expected
			} catch (const std::exception &e) {
				// A length_error or bad_alloc here means the subtraction wrapped
				// around instead of the payload being rejected up front.
				std::cout << "ShortCiphertextTest threw the wrong exception\n";
				std::cout << "  Suite: " << int(suite.cipherSuite) << ", payload: " << len
				          << ", error: " << e.what() << "\n";
				shortCiphertextFailures++;
			}
		}

		// Boundary: exactly the tag length is a well-formed empty ciphertext. It must
		// reach the crypto and fail authentication, not be rejected as malformed.
		const binary tagOnly(suite.tagLength, std::byte{0x44});
		try {
			impl::sframe::DecodeCiphertext(suite.cipherSuite, key, nonce, aad, tagOnly);
			std::cout << "ShortCiphertextTest authenticated a forged tag\n";
			std::cout << "  Suite: " << int(suite.cipherSuite) << "\n";
			shortCiphertextFailures++;
		} catch (const std::invalid_argument &e) {
			std::cout << "ShortCiphertextTest rejected a tag-length payload as malformed\n";
			std::cout << "  Suite: " << int(suite.cipherSuite) << ", error: " << e.what() << "\n";
			shortCiphertextFailures++;
		} catch (const std::runtime_error &) {
			// Expected: authentication failure
		}
	}

	// End to end over the wire-facing path: a frame whose SFrame payload is shorter
	// than the tag must be rejected cleanly by the decoder.
	const uint8_t cipherSuite = 0x04; // AES-128-GCM, 16 byte tag
	auto sendProvider =
	    std::make_shared<SFrameSendKeyProvider>(cipherSuite, 0, 0, /*perSsrcDerivation=*/false,
	                                            SFrameSendKey{binary(16, std::byte{0x55}), 0, 0});
	auto decodeKeysProvider =
	    std::make_shared<DecodeKeysProvider>(cipherSuite, binary(16, std::byte{0x55}));
	impl::SFrameDecoder decoder(decodeKeysProvider);

	binary frame = impl::sframe::header::Encode({0, 0, 0});
	frame.insert(frame.end(), 4, std::byte{0x66}); // 4 payload bytes, the tag alone needs 16

	try {
		decoder.decodeFrame(make_message(binary(frame), Message::Binary), {});
		std::cout << "ShortCiphertextTest decoder accepted a truncated GCM frame\n";
		shortCiphertextFailures++;
	} catch (const std::invalid_argument &) {
		// Expected
	} catch (const std::exception &e) {
		std::cout << "ShortCiphertextTest decoder threw the wrong exception\n";
		std::cout << "  Error: " << e.what() << "\n";
		shortCiphertextFailures++;
	}

	try {
		if (shortCiphertextFailures != 0) {
			throw std::runtime_error("Failed SFrame Short Ciphertext Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// Negative authentication tests: anything an attacker can reach must fail to
// decrypt rather than yield plaintext. Per RFC 9605 Section 4.5 the tag covers the
// nonce, the AAD (SFrame header + metadata) and the ciphertext, so mutating any
// of them, using the wrong key, or replaying a ciphertext under a different
// cipher suite must all be rejected by the crypto layer with a std::runtime_error
// ("Authentication failed" / "AES-GCM authentication failed"). std::invalid_argument
// is a logic_error, not a runtime_error, so a malformed-input rejection will not
// satisfy these checks by accident.
void test_sframe_authentication_failures() {
	struct SuiteInfo {
		uint8_t cipherSuite;
		size_t combinedKeySize; // SplitSFrameKey needs a key of exactly this size
		size_t tagLength;
	};

	// Per the IANA SFrame registry; mirrors GetCipherSuiteInfo().
	static const std::vector<SuiteInfo> suites = {
	    {0x01, 48, 10}, {0x02, 48, 8},  {0x03, 48, 4}, {0x04, 16, 16},
	    {0x05, 32, 16}, {0x06, 96, 10}, {0x07, 96, 8}, {0x08, 96, 4},
	};

	std::atomic<int> authFailures = 0;

	// Expect a decode attempt to be rejected as an authentication failure. Any
	// successful return is a break; any other exception type means the input was
	// rejected for the wrong reason and the authentication path was never reached.
	auto expectAuthFailure = [&](const string &what, uint8_t cipherSuite, const binary &key,
	                             const binary &nonce, const binary &aad, const binary &ct) {
		try {
			impl::sframe::DecodeCiphertext(cipherSuite, key, nonce, aad, ct);
			std::cout << "AuthenticationTest decrypted content that must not authenticate\n";
			std::cout << "  Suite: " << int(cipherSuite) << ", case: " << what << "\n";
			authFailures++;
		} catch (const std::runtime_error &) {
			// Expected: authentication failure
		} catch (const std::exception &e) {
			std::cout << "AuthenticationTest threw the wrong exception\n";
			std::cout << "  Suite: " << int(cipherSuite) << ", case: " << what
			          << ", error: " << e.what() << "\n";
			authFailures++;
		}
	};

	// The weaker property, for cases that legitimately need not be detected: the
	// decode must not hand back the original plaintext. In the CTR+HMAC suites the
	// tag is computed over the ciphertext with a separate authentication key, so
	// corrupting only the encryption half of the split key still authenticates and
	// returns garbage — correct encrypt-then-MAC behaviour, but it must never
	// produce the real plaintext.
	auto expectNotPlaintext = [&](const string &what, uint8_t cipherSuite, const binary &key,
	                              const binary &nonce, const binary &aad, const binary &ct,
	                              const binary &forbidden) {
		try {
			binary pt = impl::sframe::DecodeCiphertext(cipherSuite, key, nonce, aad, ct);
			if (compareEncoded(pt, forbidden)) {
				std::cout << "AuthenticationTest recovered the plaintext it must not\n";
				std::cout << "  Suite: " << int(cipherSuite) << ", case: " << what << "\n";
				authFailures++;
			}
		} catch (const std::runtime_error &) {
			// Also acceptable: rejected outright
		} catch (const std::exception &e) {
			std::cout << "AuthenticationTest threw the wrong exception\n";
			std::cout << "  Suite: " << int(cipherSuite) << ", case: " << what
			          << ", error: " << e.what() << "\n";
			authFailures++;
		}
	};

	// A genuine ciphertext per suite, kept for the cross-suite checks below.
	std::vector<binary> ciphertexts(suites.size());
	const binary plaintext(64, std::byte{0x5A});

	for (size_t s = 0; s < suites.size(); ++s) {
		const auto &suite = suites[s];
		const binary key(suite.combinedKeySize, std::byte{0x11});
		const binary nonce(12, std::byte{0x22});
		const binary aad(16, std::byte{0x33});

		binary ct = impl::sframe::EncodePlaintext(suite.cipherSuite, key, nonce, aad, plaintext);
		ciphertexts[s] = ct;

		// Sanity: the untampered ciphertext must still decrypt, otherwise the
		// negative cases below would pass for the wrong reason.
		try {
			binary pt = impl::sframe::DecodeCiphertext(suite.cipherSuite, key, nonce, aad, ct);
			if (!compareEncoded(pt, plaintext)) {
				std::cout << "AuthenticationTest baseline round-trip returned wrong plaintext\n";
				std::cout << "  Suite: " << int(suite.cipherSuite) << "\n";
				authFailures++;
			}
		} catch (const std::exception &e) {
			std::cout << "AuthenticationTest baseline round-trip failed\n";
			std::cout << "  Suite: " << int(suite.cipherSuite) << ", error: " << e.what() << "\n";
			authFailures++;
		}

		// Flip a bit in the first ciphertext byte
		binary flippedCt = ct;
		flippedCt[0] ^= std::byte{0x01};
		expectAuthFailure("bit flipped in ciphertext", suite.cipherSuite, key, nonce, aad,
		                  flippedCt);

		// Flip a bit in the last byte, which is inside the authentication tag
		binary flippedTag = ct;
		flippedTag[flippedTag.size() - 1] ^= std::byte{0x01};
		expectAuthFailure("bit flipped in tag", suite.cipherSuite, key, nonce, aad, flippedTag);

		// Tamper the AAD (in SFrame this is the header plus the metadata)
		binary tamperedAad = aad;
		tamperedAad[0] ^= std::byte{0x01};
		expectAuthFailure("tampered aad", suite.cipherSuite, key, nonce, tamperedAad, ct);

		// Wrong nonce (a replayed or forged counter)
		binary wrongNonce = nonce;
		wrongNonce[0] ^= std::byte{0x01};
		expectAuthFailure("wrong nonce", suite.cipherSuite, key, wrongNonce, aad, ct);

		// Wrong key in every byte, which corrupts the authentication half of the
		// split key as well as the encryption half
		binary wrongKey(key.size(), std::byte{0x77});
		expectAuthFailure("wrong key", suite.cipherSuite, wrongKey, nonce, aad, ct);

		// Only the encryption half wrong: the CTR+HMAC suites authenticate the
		// ciphertext with an independent key, so this is not required to be
		// detected, but it must never return the real plaintext
		binary wrongEncKey = key;
		wrongEncKey[0] ^= std::byte{0x01};
		expectNotPlaintext("wrong encryption key", suite.cipherSuite, wrongEncKey, nonce, aad, ct,
		                   plaintext);

		// Truncated by one byte: still long enough to hold a tag, so this must be
		// an authentication failure rather than a malformed-length rejection
		binary truncated(ct.begin(), ct.end() - 1);
		expectAuthFailure("truncated by one byte", suite.cipherSuite, key, nonce, aad, truncated);

		// Extra trailing byte appended
		binary extended = ct;
		extended.push_back(std::byte{0x00});
		expectAuthFailure("extra trailing byte", suite.cipherSuite, key, nonce, aad, extended);

		// Wholly attacker-supplied content of a plausible length
		binary zeros(ct.size(), std::byte{0x00});
		expectAuthFailure("all zero bytes", suite.cipherSuite, key, nonce, aad, zeros);

		binary ones(ct.size(), std::byte{0xFF});
		expectAuthFailure("all 0xFF bytes", suite.cipherSuite, key, nonce, aad, ones);
	}

	// Cross-suite confusion: a ciphertext produced under one cipher suite must not
	// authenticate when decoded as another, even with a correctly sized key for the
	// suite being claimed. The key is sized for the target suite so SplitSFrameKey
	// stays in bounds and the failure comes from the authentication check.
	for (size_t from = 0; from < suites.size(); ++from) {
		for (size_t to = 0; to < suites.size(); ++to) {
			if (from == to)
				continue;

			const auto &target = suites[to];
			const binary key(target.combinedKeySize, std::byte{0x11});
			const binary nonce(12, std::byte{0x22});
			const binary aad(16, std::byte{0x33});

			expectAuthFailure("ciphertext from suite " + std::to_string(suites[from].cipherSuite),
			                  target.cipherSuite, key, nonce, aad, ciphertexts[from]);
		}
	}

	// Frame level, over the path a tampered packet actually takes: mutating the
	// SFrame header changes the AAD and the derived nonce, so the frame must fail
	// to authenticate instead of decrypting under a different counter.
	const uint8_t cipherSuite = 0x04; // AES-128-GCM
	// A counter start of 100 rather than a single digit, so the counter occupies a field of its
	// own. RFC 9605 Section 4.3 packs a counter below 8 into the config byte's low three bits,
	// which would leave the "tampered counter" case below with no byte to flip that is not also the
	// header's flags.
	auto sendProvider =
	    std::make_shared<SFrameSendKeyProvider>(cipherSuite, 0, 0, /*perSsrcDerivation=*/false,
	                                            SFrameSendKey{binary(16, std::byte{0x55}), 0, 100});
	impl::SFrameEncoder encoder(sendProvider);
	auto decodeKeysProvider =
	    std::make_shared<DecodeKeysProvider>(cipherSuite, binary(16, std::byte{0x55}));

	const binary metadata(8, std::byte{0x44});
	message_ptr encoded =
	    encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), metadata);
	binary frame = binary((encoded)->begin(), (encoded)->end());

	// Baseline: the untampered frame decodes
	try {
		impl::SFrameDecoder decoder(decodeKeysProvider);
		message_ptr decoded =
		    decoder.decodeFrame(make_message(binary(frame), Message::Binary), metadata);
		if (!compareEncoded(binary((decoded)->begin(), (decoded)->end()), plaintext)) {
			std::cout << "AuthenticationTest frame baseline returned wrong plaintext\n";
			authFailures++;
		}
	} catch (const std::exception &e) {
		std::cout << "AuthenticationTest frame baseline failed\n";
		std::cout << "  Error: " << e.what() << "\n";
		authFailures++;
	}

	// Each frame-level mutation must be rejected
	struct FrameCase {
		string what;
		binary frame;
		binary metadata;
	};

	binary tamperedHeader = frame;
	tamperedHeader[0] ^= std::byte{0x01}; // config byte / kid and ctr lengths

	// The counter is the last field of the SFrame header, so the header's final byte carries its
	// low bits. Derived from the decoded header rather than hardcoded: the header here is 2 bytes,
	// so the offset that looks like the counter -- byte 2 -- is already the first byte of the
	// ciphertext, which would make this case a second copy of the ciphertext case below.
	const size_t headerLength = impl::sframe::header::Decode(frame).length;
	if (headerLength < 2) {
		// Only reachable if the counter went back to living inside the config byte, which would
		// leave this case tampering the flags rather than the counter.
		std::cout << "AuthenticationTest header has no counter byte to tamper\n";
		std::cout << "  Header length: " << headerLength << "\n";
		authFailures++;
	}
	binary tamperedCtr = frame;
	tamperedCtr[headerLength - 1] ^= std::byte{0x01};

	binary tamperedBody = frame;
	tamperedBody[frame.size() - 1] ^= std::byte{0x01};

	std::vector<FrameCase> frameCases = {
	    {"tampered SFrame header", tamperedHeader, metadata},
	    {"tampered counter bytes", tamperedCtr, metadata},
	    {"tampered ciphertext", tamperedBody, metadata},
	    {"tampered metadata", frame, binary(8, std::byte{0x45})},
	};

	for (const auto &frameCase : frameCases) {
		try {
			impl::SFrameDecoder decoder(decodeKeysProvider);
			decoder.decodeFrame(make_message(binary(frameCase.frame), Message::Binary),
			                    frameCase.metadata);
			std::cout << "AuthenticationTest decoded a tampered frame\n";
			std::cout << "  Case: " << frameCase.what << "\n";
			authFailures++;
		} catch (const std::exception &) {
			// Expected: the frame must not decode
		}
	}

	try {
		if (authFailures != 0) {
			throw std::runtime_error("Failed SFrame Authentication Failure Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// An empty frame is legitimate: its ciphertext is exactly the authentication tag, with no
// ciphertext bytes at all. Decrypting it must return zero bytes on every suite and every
// backend. This is a backend-parity test as much as a boundary test: a GCM path that lets
// the length written by the AAD update leak through hands back aad.size() fabricated zero
// bytes as authenticated plaintext, which the negative tests above cannot catch because
// they forge the tag and so fail authentication first.
void test_sframe_empty_plaintext() {
	struct SuiteInfo {
		uint8_t cipherSuite;
		size_t combinedKeySize;
		size_t tagLength;
	};

	static const std::vector<SuiteInfo> suites = {
	    {0x01, 48, 10}, {0x02, 48, 8},  {0x03, 48, 4}, {0x04, 16, 16},
	    {0x05, 32, 16}, {0x06, 96, 10}, {0x07, 96, 8}, {0x08, 96, 4},
	};

	// Vary the AAD length: a backend that lets the AAD length leak into the output returns
	// exactly aad.size() bytes, so a single AAD size could coincidentally look correct.
	static const std::vector<size_t> aadSizes = {0, 1, 5, 16, 40};

	std::atomic<int> emptyPlaintextFailures = 0;

	for (const auto &suite : suites) {
		const binary key(suite.combinedKeySize, std::byte{0x11});
		const binary nonce(12, std::byte{0x22});
		const binary empty;

		for (size_t aadSize : aadSizes) {
			const binary aad(aadSize, std::byte{0x33});

			binary ct = impl::sframe::EncodePlaintext(suite.cipherSuite, key, nonce, aad, empty);

			if (ct.size() != suite.tagLength) {
				std::cout << "EmptyPlaintextTest wrong ciphertext size\n";
				std::cout << "  Suite: " << int(suite.cipherSuite) << ", aad: " << aadSize
				          << ", got: " << ct.size() << ", expected: " << suite.tagLength << "\n";
				emptyPlaintextFailures++;
				continue;
			}

			try {
				binary pt = impl::sframe::DecodeCiphertext(suite.cipherSuite, key, nonce, aad, ct);
				if (!pt.empty()) {
					std::cout << "EmptyPlaintextTest fabricated plaintext bytes\n";
					std::cout << "  Suite: " << int(suite.cipherSuite) << ", aad: " << aadSize
					          << ", got: " << pt.size() << " bytes, expected: 0\n";
					emptyPlaintextFailures++;
				}
			} catch (const std::exception &e) {
				std::cout << "EmptyPlaintextTest failed to authenticate an empty frame\n";
				std::cout << "  Suite: " << int(suite.cipherSuite) << ", aad: " << aadSize
				          << ", error: " << e.what() << "\n";
				emptyPlaintextFailures++;
			}
		}
	}

	try {
		if (emptyPlaintextFailures != 0) {
			throw std::runtime_error("Failed SFrame Empty Plaintext Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// RFC 9605 Section 9.1: a (key, CTR) pair must never be reused. impl::SFrameEncoder claims the key
// derived key and the counter together under one lock because Track::outgoing() runs the chain on
// the caller's thread, so concurrent send() calls reach encodeFrame() in parallel.
// Two frames sharing a counter share a nonce, which for the GCM suites leaks the GHASH key.
void test_sframe_counter_uniqueness() {
	std::atomic<int> counterFailures = 0;

	// Sequential: the counter starts at ctrStart and advances by one per frame
	{
		const uint64_t ctrStart = 100;
		impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
		    0x04, 0, 0, /*perSsrcDerivation=*/false,
		    SFrameSendKey{binary(16, std::byte{0x55}), 0, ctrStart}));

		if (encoder.currentCounter() != ctrStart) {
			std::cout << "CounterTest wrong initial counter\n";
			std::cout << "  Got: " << encoder.currentCounter() << ", expected: " << ctrStart
			          << "\n";
			counterFailures++;
		}

		const binary frame(32, std::byte{0x66});
		for (uint64_t i = 0; i < 4; ++i) {
			message_ptr encoded =
			    encoder.encodeFrame(make_message(binary(frame), Message::Binary), {});
			impl::SFrameHeaderInfo header =
			    impl::sframe::header::Decode(binary((encoded)->begin(), (encoded)->end()));
			if (header.ctr != ctrStart + i) {
				std::cout << "CounterTest wrong counter on the wire\n";
				std::cout << "  Frame: " << i << ", got: " << header.ctr
				          << ", expected: " << ctrStart + i << "\n";
				counterFailures++;
			}
		}

		if (encoder.currentCounter() != ctrStart + 4) {
			std::cout << "CounterTest counter did not advance\n";
			std::cout << "  Got: " << encoder.currentCounter() << ", expected: " << ctrStart + 4
			          << "\n";
			counterFailures++;
		}
	}

	// Concurrent: every frame produced by a shared encoder must carry a distinct counter
	{
		const unsigned threadCount = 8;
		const unsigned framesPerThread = 64;

		impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
		    0x04, 0, 0, /*perSsrcDerivation=*/false,
		    SFrameSendKey{binary(16, std::byte{0x55}), 0, 0}));

		std::mutex resultMutex;
		std::vector<impl::SFrameHeaderInfo> headers;
		headers.reserve(threadCount * framesPerThread);

		std::vector<std::thread> threads;
		for (unsigned t = 0; t < threadCount; ++t) {
			threads.emplace_back([&, t] {
				const binary frame(32, std::byte(t));
				std::vector<impl::SFrameHeaderInfo> local;
				local.reserve(framesPerThread);
				for (unsigned f = 0; f < framesPerThread; ++f) {
					message_ptr encoded =
					    encoder.encodeFrame(make_message(binary(frame), Message::Binary), {});
					local.push_back(
					    impl::sframe::header::Decode(binary((encoded)->begin(), (encoded)->end())));
				}
				std::lock_guard<std::mutex> lock(resultMutex);
				headers.insert(headers.end(), local.begin(), local.end());
			});
		}
		for (auto &thread : threads)
			thread.join();

		const size_t expected = size_t(threadCount) * framesPerThread;
		if (headers.size() != expected) {
			std::cout << "CounterTest wrong frame count\n";
			std::cout << "  Got: " << headers.size() << ", expected: " << expected << "\n";
			counterFailures++;
		}

		std::set<std::pair<uint64_t, uint64_t>> seen;
		size_t duplicates = 0;
		for (const auto &header : headers) {
			if (!seen.insert({header.kid, header.ctr}).second)
				duplicates++;
		}

		if (duplicates != 0) {
			std::cout << "CounterTest reused a (kid, ctr) pair, so a nonce was reused\n";
			std::cout << "  Duplicates: " << duplicates << " of " << headers.size() << "\n";
			counterFailures++;
		}
	}

	// A ratchet advances the KID, which is an HKDF input to both the key and the salt, so the
	// counter restarts rather than climbing. (kid, ctr) is still unique across the change, and
	// that is the pair RFC 9605 Section 9.1 requires be unique -- not ctr alone.
	{
		// Ratcheting needs a ratchet field to advance within; kid = generation 1, step 0.
		const uint8_t ratchetStepBits = 4;
		impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
		    0x04, ratchetStepBits, /*ratchetPeriod=*/1, /*perSsrcDerivation=*/false,
		    SFrameSendKey{binary(16, std::byte{0x55}), uint64_t(1) << ratchetStepBits}));

		const binary frame(32, std::byte{0x66});
		message_ptr before = encoder.encodeFrame(make_message(binary(frame), Message::Binary), {});
		impl::SFrameHeaderInfo beforeHeader =
		    impl::sframe::header::Decode(binary((before)->begin(), (before)->end()));

		std::this_thread::sleep_for(std::chrono::milliseconds(1100));

		message_ptr after = encoder.encodeFrame(make_message(binary(frame), Message::Binary), {});
		impl::SFrameHeaderInfo afterHeader =
		    impl::sframe::header::Decode(binary((after)->begin(), (after)->end()));

		if (afterHeader.kid != beforeHeader.kid + 1) {
			std::cout << "CounterTest ratchet did not advance the kid\n";
			std::cout << "  Before: " << beforeHeader.kid << ", after: " << afterHeader.kid << "\n";
			counterFailures++;
		}

		if (afterHeader.ctr != 0) {
			std::cout << "CounterTest ratchet did not restart the counter under the new kid\n";
			std::cout << "  Got: " << afterHeader.ctr << ", expected 0\n";
			counterFailures++;
		}

		// Which is only safe because the pair differs, so spell that out.
		if (afterHeader.kid == beforeHeader.kid && afterHeader.ctr == beforeHeader.ctr) {
			std::cout << "CounterTest reused a (kid, ctr) pair across a ratchet\n";
			counterFailures++;
		}
	}

	// An empty base key derives a key the attacker can derive too, so it must be rejected
	{
		bool threw = false;
		try {
			impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
			    0x04, 0, 0, /*perSsrcDerivation=*/false, SFrameSendKey{binary(), 0, 0}));
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		if (!threw) {
			std::cout << "CounterTest accepted an empty base key\n";
			counterFailures++;
		}
	}

	try {
		if (counterFailures != 0) {
			throw std::runtime_error("Failed SFrame Counter Uniqueness Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// sframe::header, impl::SFrameDecoder and sframe are RTC_CPP_EXPORT and reachable by library
// consumers, so they must not index into caller-supplied buffers unchecked. The in-tree RTP
// paths happen to guarantee at least one byte, but an out-of-bounds read here is not
// something the try/catch in DecryptMessages could contain.
void test_sframe_bounds_checks() {
	std::atomic<int> boundsFailures = 0;

	auto expectThrow = [&](const string &what, const std::function<void()> &fn) {
		try {
			fn();
			std::cout << "BoundsTest accepted malformed input\n";
			std::cout << "  Case: " << what << "\n";
			boundsFailures++;
		} catch (const std::exception &) {
			// Expected: rejected rather than read out of bounds
		}
	};

	// An empty buffer has no config byte
	expectThrow("empty header", [] { impl::sframe::header::Decode({}); });

	// Extended KID/CTR lengths that run past the end of the buffer. The config byte
	// declares an 8-byte extended KID and an 8-byte extended CTR, but no bytes follow.
	expectThrow("truncated extended kid",
	            [] { impl::sframe::header::Decode(binary{std::byte{0xF0}}); });
	expectThrow("truncated extended ctr",
	            [] { impl::sframe::header::Decode(binary{std::byte{0x0F}}); });
	expectThrow("truncated extended kid and ctr", [] {
		impl::sframe::header::Decode(
		    binary{std::byte{0xFF}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}});
	});

	// A nonce of the wrong length. OpenSSL's EVP_EncryptInit_ex reads its cipher's own IV length
	// regardless of what it is handed, so a short nonce is a heap over-read in gcm_encrypt rather
	// than an error -- the other three backends merely fail, which is why no backend-parity test
	// would surface this. Every other nonce in this file is exactly 12 bytes, the size all eight
	// registered suites use, so without the cases below the guard is unreachable from the suite.
	{
		const uint16_t suite = 0x0004; // AES-128-GCM: 16-byte combined key, 12-byte nonce
		const binary key(16, std::byte{0x2B});
		const binary aad(4, std::byte{0x01});
		const binary plaintext(32, std::byte{0x7C});

		expectThrow("nonce one byte short", [&] {
			impl::sframe::EncodePlaintext(suite, key, binary(11, std::byte{0x09}), aad, plaintext);
		});
		expectThrow("nonce one byte long", [&] {
			impl::sframe::EncodePlaintext(suite, key, binary(13, std::byte{0x09}), aad, plaintext);
		});
		expectThrow("empty nonce", [&] {
			impl::sframe::EncodePlaintext(suite, key, {}, aad, plaintext);
		});
		expectThrow("short nonce on decode", [&] {
			impl::sframe::DecodeCiphertext(suite, key, binary(11, std::byte{0x09}), aad,
			                               binary(48, std::byte{0x00}));
		});

		// The correct length still works, so none of the above is a blanket refusal.
		try {
			auto ct = impl::sframe::EncodePlaintext(suite, key, binary(12, std::byte{0x09}), aad,
			                                        plaintext);
			if (ct.size() <= plaintext.size()) {
				std::cout << "BoundsTest a correct nonce produced no tag\n";
				boundsFailures++;
			}
		} catch (const std::exception &e) {
			std::cout << "BoundsTest rejected a correctly sized nonce: " << e.what() << "\n";
			boundsFailures++;
		}
	}

	// ConstantTimeEquals must compare lengths before it compares bytes: without that it reads
	// a.size() bytes out of the shorter buffer. Unequal lengths are a live production input --
	// rollKey() compares caller-supplied material against the stored key, so an application moving
	// from AES-128 to AES-256 material hits this on the first roll.
	{
		const binary shortKey(16, std::byte{0x5A});
		const binary longKey(32, std::byte{0x5A});
		if (impl::sframe::ConstantTimeEquals(shortKey, longKey) ||
		    impl::sframe::ConstantTimeEquals(longKey, shortKey)) {
			std::cout << "BoundsTest ConstantTimeEquals matched buffers of different lengths\n";
			boundsFailures++;
		}
		if (impl::sframe::ConstantTimeEquals(binary(16, std::byte{0x5A}), {}) ||
		    impl::sframe::ConstantTimeEquals({}, binary(16, std::byte{0x5A}))) {
			std::cout << "BoundsTest ConstantTimeEquals matched against an empty buffer\n";
			boundsFailures++;
		}
		// Equal length and equal content still matches, so the guard is not answering false to
		// everything -- which would satisfy both checks above.
		if (!impl::sframe::ConstantTimeEquals(shortKey, binary(16, std::byte{0x5A}))) {
			std::cout << "BoundsTest ConstantTimeEquals rejected two identical buffers\n";
			boundsFailures++;
		}
	}

	// A base key shorter than the suite's combined key size. The CTR+HMAC suites split it into an
	// encryption key and an auth key, so a short one is constructed from an iterator past the end.
	{
		// AES-128-CTR-HMAC-SHA256: a 48-byte combined key split into a 16-byte encryption key and
		// a 32-byte auth key, so a short one is constructed from an iterator past the end.
		const uint16_t ctrSuite = 0x0001;
		expectThrow("base key too short to split", [&] {
			impl::sframe::EncodePlaintext(ctrSuite, binary(8, std::byte{0x33}),
			                              binary(12, std::byte{0x09}), binary(4, std::byte{0x01}),
			                              binary(32, std::byte{0x7C}));
		});
	}

	// A salt shorter than the nonce would be read past its end
	expectThrow("salt shorter than nonce", [] {
		impl::sframe::DeriveNonce(binary(4, std::byte{0x11}), /*ctr=*/1, /*nonceSize=*/12);
	});
	expectThrow("empty salt", [] { impl::sframe::DeriveNonce({}, /*ctr=*/1, /*nonceSize=*/12); });

	// A well-formed salt still works, so the guard is not a blanket refusal
	try {
		auto nonce = impl::sframe::DeriveNonce(binary(12, std::byte{0x11}), 1, 12);
		if (nonce.size() != 12) {
			std::cout << "BoundsTest DeriveNonce returned the wrong size\n";
			std::cout << "  Got: " << nonce.size() << ", expected: 12\n";
			boundsFailures++;
		}
	} catch (const std::exception &e) {
		std::cout << "BoundsTest rejected a well-formed salt\n";
		std::cout << "  Error: " << e.what() << "\n";
		boundsFailures++;
	}

	// The decoder reaches Decode() with whatever arrived on the wire, including nothing
	{
		auto provider = std::make_shared<DecodeKeysProvider>(0x04, binary(16, std::byte{0x55}));
		impl::SFrameDecoder decoder(provider);
		expectThrow("decode an empty frame",
		            [&] { decoder.decodeFrame(make_message(binary({}), Message::Binary), {}); });
	}

	try {
		if (boundsFailures != 0) {
			throw std::runtime_error("Failed SFrame Bounds Check Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// RFC 9605 Section 5.1 splits the KID as
//     KID = (key_generation << R) + (ratchet_step % (1 << R))
// Ratcheting advances only the low R bits: incrementing the whole KID would walk out of
// the range the key management system allocated to this sender.
void test_sframe_ratchet_kid_field() {
	std::atomic<int> ratchetFailures = 0;

	// Hand-computed values first. The round-trip check below composes and decomposes with the
	// same pair of functions, so a wrong shift in both would agree with itself and pass.
	{
		struct Vector {
			uint64_t generation;
			uint64_t step;
			uint8_t bits;
			uint64_t kid;
		};
		// kid = (generation << bits) | step, per RFC 9605 Section 5.1.
		const Vector vectors[] = {
		    {0x1234, 0x56, 8, 0x123456}, {0, 0, 8, 0},    {1, 0, 8, 0x100},
		    {0, 0xFF, 8, 0xFF},          {7, 3, 4, 0x73}, {1, 0, 1, 2},
		    {0x2A, 0xFFF, 12, 0x2AFFF},  {0, 1, 32, 1},   {1, 0, 32, 0x100000000ULL},
		};
		for (const auto &v : vectors) {
			const uint64_t got = impl::sframe::MakeKid(v.generation, v.step, v.bits);
			if (got != v.kid) {
				std::cout << "RatchetKidTest MakeKid does not match the specified layout\n";
				std::cout << "  generation " << v.generation << ", step " << v.step << ", R "
				          << unsigned(v.bits) << ": got " << got << ", expected " << v.kid << "\n";
				ratchetFailures++;
			}
		}

		// A step wider than the field is taken modulo 2^R rather than overflowing into the
		// generation. The generation must have a clear low bit for that to be visible: with
		// generation 6, step 0x1FF and R=8, masking gives 0x6FF and overflow gives 0x7FF.
		// Generation 7 would give 0x7FF either way, since its low bit already covers the
		// overflowing one.
		const uint64_t masked = impl::sframe::MakeKid(6, 0x1FF, 8);
		if (masked != 0x6FF) {
			std::cout << "RatchetKidTest an over-wide step leaked into the key generation\n";
			std::cout << "  Got: " << masked << ", expected " << 0x6FF << "\n";
			ratchetFailures++;
		}
	}

	// The two halves round-trip
	{
		const uint8_t bits = 8;
		const uint64_t kid = impl::sframe::MakeKid(/*generation=*/0x1234, /*step=*/0x56, bits);
		if (impl::sframe::KeyGenerationFromKid(kid, bits) != 0x1234 ||
		    impl::sframe::RatchetStepFromKid(kid, bits) != 0x56) {
			std::cout << "RatchetKidTest kid did not decompose back to its parts\n";
			std::cout << "  kid: " << kid << "\n";
			ratchetFailures++;
		}
	}

	// Walking the field: the step advances, wraps to zero, and never touches the
	// generation above it
	{
		const uint8_t bits = 4;
		const uint64_t generation = 7;
		uint64_t kid = impl::sframe::MakeKid(generation, 0, bits);

		for (uint64_t step = 1; step <= (uint64_t(1) << bits); ++step) {
			kid = impl::sframe::NextRatchetKid(kid, bits);

			const uint64_t expectedStep = step % (uint64_t(1) << bits);
			if (impl::sframe::RatchetStepFromKid(kid, bits) != expectedStep) {
				std::cout << "RatchetKidTest wrong ratchet step\n";
				std::cout << "  After " << step << " ratchets, got "
				          << impl::sframe::RatchetStepFromKid(kid, bits) << ", expected "
				          << expectedStep << "\n";
				ratchetFailures++;
			}
			if (impl::sframe::KeyGenerationFromKid(kid, bits) != generation) {
				std::cout << "RatchetKidTest ratchet escaped into the key generation\n";
				std::cout << "  After " << step << " ratchets, generation is "
				          << impl::sframe::KeyGenerationFromKid(kid, bits) << ", expected "
				          << generation << "\n";
				ratchetFailures++;
			}
		}

		// A full wrap returns to the starting KID
		if (kid != impl::sframe::MakeKid(generation, 0, bits)) {
			std::cout << "RatchetKidTest full wrap did not return to the starting kid\n";
			ratchetFailures++;
		}
	}

	// An encoder ratchets within its field rather than out of it. The starting step is the
	// last one in the field, so a single ratchet has to wrap: away from the wrap, adding
	// one to the whole KID and adding one to the step are indistinguishable, and only the
	// wrap shows whether the ratchet is confined to its bits.
	{
		const uint8_t bits = 4;
		const uint64_t generation = 3;
		const uint64_t lastStep = (uint64_t(1) << bits) - 1;
		// Starting at the last step in the field, so the next ratchet must wrap.
		impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
		    0x04, bits, /*ratchetPeriod=*/1, /*perSsrcDerivation=*/false,
		    SFrameSendKey{binary(16, std::byte{0x55}),
		                  impl::sframe::MakeKid(generation, lastStep, bits)}));

		const binary frame(32, std::byte{0x66});
		auto kidOf = [&] {
			message_ptr encoded =
			    encoder.encodeFrame(make_message(binary(frame), Message::Binary), {});
			return impl::sframe::header::Decode(binary((encoded)->begin(), (encoded)->end())).kid;
		};

		const uint64_t before = kidOf();
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		const uint64_t after = kidOf();

		if (impl::sframe::KeyGenerationFromKid(after, bits) != generation) {
			std::cout << "RatchetKidTest encoder ratcheted out of its key generation\n";
			std::cout << "  Before: " << before << ", after: " << after << "\n";
			ratchetFailures++;
		}
		if (impl::sframe::RatchetStepFromKid(after, bits) != 0) {
			std::cout << "RatchetKidTest encoder did not wrap the ratchet step to zero\n";
			std::cout << "  Before: " << before << ", after: " << after << "\n";
			ratchetFailures++;
		}
	}

	// Rejected configurations
	{
		auto rejects = [&](const string &what, const std::function<void()> &fn) {
			try {
				fn();
				std::cout << "RatchetKidTest accepted " << what << "\n";
				ratchetFailures++;
			} catch (const std::invalid_argument &) {
				// Expected
			}
		};

		rejects("ratchetStepBits above the maximum", [] {
			impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
			    0x04, uint8_t(SFrameMaxRatchetStepBits + 1), /*ratchetPeriod=*/0,
			    /*perSsrcDerivation=*/false, SFrameSendKey{binary(16, std::byte{0x55}), 0}));
		});

		rejects("a ratchet period with no ratchet field", [] {
			impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
			    0x04, /*ratchetStepBits=*/0, /*ratchetPeriod=*/1, /*perSsrcDerivation=*/false,
			    SFrameSendKey{binary(16, std::byte{0x55}), 0}));
		});
	}

	try {
		if (ratchetFailures != 0) {
			throw std::runtime_error("Failed SFrame Ratchet KID Field Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// Per-SSRC key derivation and ratcheting against the vectors from the SFrame reference
// implementation, sframe-wg/sframe PR 207
// (test-vectors/test-vectors-per-ssrc-key-derivation.json).
//
// This is the construction draft-ietf-avtcore-rtp-sframe sections 7 and 8 specify and RFC 9605
// does not, so these are the only published values that pin it -- a round-trip test cannot,
// because both sides would agree on a wrong answer.
void test_sframe_per_ssrc_key_derivation() {
	std::atomic<int> ssrcFailures = 0;

	struct PerSsrcVector {
		std::vector<uint16_t> cipherSuites;
		string baseKey;
		SSRC ssrc;
		string ssrcKey;  // as derived from the base key
		string ssrcKey1; // after one ratchet step
		string ssrcKey2; // after two
	};

	const string baseKey16 = "000102030405060708090a0b0c0d0e0f";
	const string baseKey32 = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

	const std::vector<PerSsrcVector> vectors = {
	    {{0x01, 0x02, 0x03, 0x04},
	     baseKey16,
	     0x12345678,
	     "f309f118aecdacfbc165cc080022b297100bbb55e483b002d762000a0faa684d",
	     "a8c48a47a6536eda200b4e94e8eaa6602f4f2266bd66284490229540f93127ab",
	     "1141095c432877af08c314e0e2936ce4389d697c74769fb2db911ea467f984dc"},
	    {{0x01, 0x02, 0x03, 0x04},
	     baseKey16,
	     0x98765432,
	     "06e5d1b8c6e01f10b592bd2e13ff4b37182b895ec8330d5cc6ec690704043b5f",
	     "7aa3bf333dde545fe56daaf2449fbff1774f6df78ce0b593b59f05f3d1f505dd",
	     "4aa1c3f077b45d4d76cb470690529534721b21eda62bceaa8ec73e59926d25c4"},
	    {{0x05, 0x06, 0x07, 0x08},
	     baseKey32,
	     0x12345678,
	     "4715e2a3e3ecb408c83135a30b8e660c75a3488dbd4d9dd3553a01b39df76bc4"
	     "3a5a0b130159edd90353b09c810e849db7398182ac7c1dea072a2c1b6bac7dda",
	     "9f0f48590c721b61733d1021212d6b92953e8aa73e9fac510e33113be7abb3a1"
	     "230daf15bc5c5247ffeab88f745ca59eba608e832e8e9b5a50cd05c5b866e604",
	     "b10d573c2a0447596d8a162093d7c0aa0fe0eb84a598d06174b24e8ab2ca73a8"
	     "1dedb3158a6ea9a734e5544769524766325a4248e585c8231a323f2093afc118"},
	    {{0x05, 0x06, 0x07, 0x08},
	     baseKey32,
	     0x98765432,
	     "f7891c6b18d88a93ba7638875b87146df4cb9693ca3837b480e9221539e86c1c"
	     "98391cb1273423b33f94abe7f67c1070997a588084a54391ebda18b3d6ff92f1",
	     "686debc361271a29bb6e908ca4cbae6a8d2512ba73b89bbb883a81d6aceab6bf"
	     "69e24064ae2758803ac4f2ff0aab2bd8cf1b4eab1e608892e816e4b4d6374b71",
	     "ab9a74c7040713e8d1c3aa4ab5c6b22e214c81d571c182b7d83b480364e86c9c"
	     "d99bff7af9e1abc9e6ef772460835aa420b30d9329b9f686558e6e601eb4615c"},
	};

	for (const auto &vector : vectors) {
		const binary baseKey = convertFromHex(vector.baseKey);

		for (uint16_t cipherSuite : vector.cipherSuites) {
			// Walk the chain: derive once from the base key, then ratchet that key. Each
			// step is compared, so a break is reported at the step it happens.
			const string *expectedHex[] = {&vector.ssrcKey, &vector.ssrcKey1, &vector.ssrcKey2};
			binary key;
			bool broken = false;

			for (size_t step = 0; step < 3 && !broken; ++step) {
				try {
					key = step == 0 ? impl::sframe::DeriveSSRCKey(vector.ssrc, baseKey, cipherSuite)
					                : impl::sframe::RatchetKey(cipherSuite, key);
				} catch (const std::exception &e) {
					std::cout << "PerSsrcKeyDerivationTest threw at step " << step << " for suite "
					          << cipherSuite << "\n";
					std::cout << "  Error: " << e.what() << "\n";
					ssrcFailures++;
					broken = true;
					break;
				}

				const binary expected = convertFromHex(*expectedHex[step]);
				if (key.size() != expected.size()) {
					std::cout << "PerSsrcKeyDerivationTest wrong key length at step " << step
					          << " for suite " << cipherSuite << "\n";
					std::cout << "  Expected: " << expected.size() << ", got: " << key.size()
					          << "\n";
					ssrcFailures++;
					broken = true;
					break;
				}

				if (!compareEncoded(key, expected)) {
					std::cout << "PerSsrcKeyDerivationTest wrong key at ratchet step " << step
					          << " for suite " << cipherSuite << ", ssrc " << vector.ssrc << "\n";
					std::cout << "  Expected: " << *expectedHex[step] << "\n";
					std::cout << "  Got:      " << convertToHex(key) << "\n";
					ssrcFailures++;
					broken = true;
				}
			}
		}
	}

	// Distinct SSRCs must not share a key, which is the whole point of the derivation.
	{
		const binary baseKey = convertFromHex(baseKey16);
		auto a = impl::sframe::DeriveSSRCKey(0x12345678, baseKey, 0x04);
		auto b = impl::sframe::DeriveSSRCKey(0x98765432, baseKey, 0x04);
		if (compareEncoded(a, b)) {
			std::cout << "PerSsrcKeyDerivationTest two SSRCs derived the same key\n";
			ssrcFailures++;
		}
	}

	// Ratcheting the base key and then deriving per-SSRC is a different chain from
	// deriving then ratcheting. Section 8 requires the latter, and the two must not be
	// confused: this is the ordering the vectors above pin at every step.
	{
		const binary baseKey = convertFromHex(baseKey16);
		auto derivedThenRatcheted =
		    impl::sframe::RatchetKey(0x04, impl::sframe::DeriveSSRCKey(0x12345678, baseKey, 0x04));
		auto ratchetedThenDerived =
		    impl::sframe::DeriveSSRCKey(0x12345678, impl::sframe::RatchetKey(0x04, baseKey), 0x04);
		if (compareEncoded(derivedThenRatcheted, ratchetedThenDerived)) {
			std::cout << "PerSsrcKeyDerivationTest ratchet and per-SSRC derivation commute, "
			             "so the section 8 ordering is not being tested\n";
			ssrcFailures++;
		}
	}

	try {
		if (ssrcFailures != 0) {
			throw std::runtime_error("Failed SFrame Per-SSRC Key Derivation Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// The suites this implementation knows are 0x01-0x08, all of them in the IANA SFrame registry
// (https://www.iana.org/assignments/sframe), which names the defining document for each. Anything else has no key size, nonce size or tag
// length to work from, so it must be refused rather than silently treated as some default --
// 0 in particular is what an uninitialised field carries.
static void test_sframe_cipher_suite_validation() {
	std::atomic<int> suiteFailures = 0;
	const binary key(32, std::byte{0x5A});

	for (uint16_t suite : {uint16_t(0x00), uint16_t(0x09), uint16_t(0xFF), uint16_t(0x1234)}) {
		bool threw = false;
		try {
			auto provider = std::make_shared<SFrameSendKeyProvider>(
			    suite, 0, 0, /*perSsrcDerivation=*/false, SFrameSendKey{key, 0});
		} catch (const std::invalid_argument &) {
			// By type, not std::exception: the header promises invalid_argument for every rejection
			// the constructors make, and runtime_error is a different hierarchy to catch.
			threw = true;
		}
		if (!threw) {
			std::cout << "CipherSuiteTest encoder accepted an unregistered cipher suite\n";
			std::cout << "  Suite: " << suite << "\n";
			suiteFailures++;
		}
	}

	// Every suite the implementation knows constructs, so the check is not rejecting everything.
	for (uint16_t suite = 0x01; suite <= 0x08; ++suite) {
		const binary sized(suite >= 0x05 ? 32 : 16, std::byte{0x5A});
		try {
			auto provider = std::make_shared<SFrameSendKeyProvider>(
			    suite, 0, 0, /*perSsrcDerivation=*/false, SFrameSendKey{sized, 0});
		} catch (const std::exception &e) {
			std::cout << "CipherSuiteTest rejected a registered cipher suite\n";
			std::cout << "  Suite: " << suite << ", error: " << e.what() << "\n";
			suiteFailures++;
		}
	}

	// And the receive side: an unregistered suite is refused when the provider is built, and a
	// decoder built for a registered suite the frame was not encrypted under must not decode it.
	{
		impl::SFrameEncoder encoder(
		    std::make_shared<SFrameSendKeyProvider>(0x04, 0, 0, /*perSsrcDerivation=*/false,
		                                            SFrameSendKey{binary(16, std::byte{0x5A}), 0}));
		auto frame =
		    encoder.encodeFrame(make_message(binary(64, std::byte{0x11}), Message::Binary), {});

		// An unregistered suite is refused when the provider is built, so a decoder can never
		// be handed one -- which is stricter than catching it per frame.
		bool providerThrew = false;
		try {
			auto bad = std::make_shared<SFrameReceiveKeyProvider>(0x09, 0, false);
		} catch (const std::exception &) {
			providerThrew = true;
		}
		if (!providerThrew) {
			std::cout << "CipherSuiteTest a receive provider accepted an unregistered suite\n";
			suiteFailures++;
		}

		auto provider = std::make_shared<DecodeKeysProvider>(0x05, binary(32, std::byte{0x5A}));
		impl::SFrameDecoder decoder(provider);
		bool threw = false;
		try {
			decoder.decodeFrame(frame, {});
		} catch (const std::exception &) {
			threw = true;
		}
		if (!threw) {
			std::cout << "CipherSuiteTest decoder used an unregistered cipher suite\n";
			suiteFailures++;
		}
	}

	if (suiteFailures > 0)
		throw std::runtime_error("Cipher suite validation failures: " +
		                         std::to_string(suiteFailures.load()));
}

void test_sframe_key_validation() {
	std::atomic<int> keyFailures = 0;

	const size_t minSize = impl::sframe::MinBaseKeySize();
	if (minSize < 16) {
		std::cout << "KeyValidationTest minimum base key size is below 128 bits\n";
		std::cout << "  Got: " << minSize << "\n";
		keyFailures++;
	}

	// Every length below the floor is rejected, including empty
	for (size_t len = 0; len < minSize; ++len) {
		try {
			impl::sframe::ValidateBaseKey(binary(len, std::byte{0x11}));
			std::cout << "KeyValidationTest accepted a short base key\n";
			std::cout << "  Length: " << len << ", minimum: " << minSize << "\n";
			keyFailures++;
		} catch (const std::invalid_argument &) {
			// Expected
		}
	}

	// The floor itself is accepted
	try {
		impl::sframe::ValidateBaseKey(binary(minSize, std::byte{0x11}));
	} catch (const std::exception &e) {
		std::cout << "KeyValidationTest rejected a base key at the minimum size\n";
		std::cout << "  Error: " << e.what() << "\n";
		keyFailures++;
	}

	// The encoder applies the same rule at construction
	{
		bool threw = false;
		try {
			impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
			    0x04, 0, 0, /*perSsrcDerivation=*/false,
			    SFrameSendKey{binary(minSize - 1, std::byte{0x55}), 0, 0}));
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		if (!threw) {
			std::cout << "KeyValidationTest encoder accepted a short base key\n";
			keyFailures++;
		}
	}

	// Both derivation entry points reject it, so no path reaches the KDF with a weak key
	{
		bool threw = false;
		try {
			impl::sframe::GetSFrameKeys(/*kid=*/1, /*ctr=*/0, binary(), 0x04);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		if (!threw) {
			std::cout << "KeyValidationTest GetSFrameKeys derived from an empty base key\n";
			keyFailures++;
		}
	}

	{
		bool threw = false;
		try {
			impl::sframe::DeriveSSRCKey(0x1234, binary(), 0x04);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		if (!threw) {
			std::cout << "KeyValidationTest DeriveSSRCKey derived from an empty base key\n";
			keyFailures++;
		}
	}

	// A frame whose KID the provider does not recognise must be refused, not decoded
	// against whatever the provider returns as a fallback.
	{
		const uint8_t cipherSuite = 0x04;
		const binary baseKey(16, std::byte{0x55});
		const binary plaintext(32, std::byte{0x66});

		impl::SFrameEncoder encoder(std::make_shared<SFrameSendKeyProvider>(
		    cipherSuite, 0, 0, /*perSsrcDerivation=*/false, SFrameSendKey{baseKey, /*kid=*/1}));
		message_ptr encoded =
		    encoder.encodeFrame(make_message(binary(plaintext), Message::Binary), {});
		binary frame = binary((encoded)->begin(), (encoded)->end());

		// Baseline: the provider that knows kid 1 decodes it
		try {
			impl::SFrameDecoder decoder(
			    std::make_shared<DecodeKeysProvider>(cipherSuite, baseKey, 1));
			message_ptr out = decoder.decodeFrame(make_message(binary(frame), Message::Binary), {});
			if (!compareEncoded(binary((out)->begin(), (out)->end()), plaintext)) {
				std::cout << "KeyValidationTest baseline returned the wrong plaintext\n";
				keyFailures++;
			}
		} catch (const std::exception &e) {
			std::cout << "KeyValidationTest baseline failed\n";
			std::cout << "  Error: " << e.what() << "\n";
			keyFailures++;
		}

		// A provider that only knows a different KID answers nullopt
		try {
			impl::SFrameDecoder decoder(
			    std::make_shared<DecodeKeysProvider>(cipherSuite, baseKey, 2));
			decoder.decodeFrame(make_message(binary(frame), Message::Binary), {});
			std::cout << "KeyValidationTest decoded a frame for an unknown key generation\n";
			keyFailures++;
		} catch (const std::runtime_error &) {
			// Expected: no key for this kid
		} catch (const std::exception &e) {
			std::cout
			    << "KeyValidationTest threw the wrong exception for an unknown key generation\n";
			std::cout << "  Error: " << e.what() << "\n";
			keyFailures++;
		}

		// A provider that answers with an empty base key instead of nullopt must not
		// produce a usable key either
		try {
			impl::SFrameDecoder decoder(std::make_shared<EmptyKeyProvider>(cipherSuite));
			decoder.decodeFrame(make_message(binary(frame), Message::Binary), {});
			std::cout << "KeyValidationTest decoded a frame with an empty base key\n";
			keyFailures++;
		} catch (const std::invalid_argument &) {
			// Expected: the base key is refused before any key is derived
		} catch (const std::exception &e) {
			std::cout << "KeyValidationTest threw the wrong exception for an empty base key\n";
			std::cout << "  Error: " << e.what() << "\n";
			keyFailures++;
		}
	}

	try {
		if (keyFailures != 0) {
			throw std::runtime_error("Failed SFrame Key Validation Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// What zeroization can be asserted from inside the process. The point of it -- that a discarded key
// is not left readable in freed heap -- cannot be: reading a buffer the allocator has taken back is
// undefined, so a test that "proves" it is really testing the allocator. What is checkable is the
// contract each piece offers on a buffer that is still alive, and the self-assignment case, where
// clearing before assigning would destroy the very key it was meant to keep.
void test_sframe_key_cleansing() {
	std::atomic<int> cleanseFailures = 0;

	const binary material(32, std::byte{0xA5});

	// Clears the bytes and leaves the length alone: callers hand these buffers straight back to
	// code that reads size().
	{
		binary buffer = material;
		impl::sframe::Cleanse(buffer);

		if (buffer.size() != material.size()) {
			std::cout << "CleanseTest changed the buffer length\n";
			std::cout << "  Got: " << buffer.size() << ", expected: " << material.size() << "\n";
			cleanseFailures++;
		}
		if (buffer != binary(material.size(), std::byte{0})) {
			std::cout << "CleanseTest left non-zero bytes behind\n";
			cleanseFailures++;
		}
	}

	// An empty buffer's data() may be null, which the backends must not be handed.
	{
		binary empty;
		try {
			impl::sframe::Cleanse(empty);
		} catch (const std::exception &e) {
			std::cout << "CleanseTest threw on an empty buffer\n";
			std::cout << "  Error: " << e.what() << "\n";
			cleanseFailures++;
		}
	}

	// ScopedCleanse clears on the way out, and the buffer outlives the guard, so this one is
	// directly observable rather than inferred.
	{
		binary buffer = material;
		{
			impl::sframe::ScopedCleanse guard(buffer);
		}

		if (buffer != binary(material.size(), std::byte{0})) {
			std::cout << "CleanseTest ScopedCleanse did not clear on scope exit\n";
			cleanseFailures++;
		}
	}

	// Clears even when the scope is left by an exception, which is the case the guard exists for:
	// key derivation throws on bad material.
	{
		binary buffer = material;
		try {
			impl::sframe::ScopedCleanse guard(buffer);
			throw std::runtime_error("unwinding");
		} catch (const std::runtime_error &) {
			// Expected
		}

		if (buffer != binary(material.size(), std::byte{0})) {
			std::cout << "CleanseTest ScopedCleanse did not clear while unwinding\n";
			cleanseFailures++;
		}
	}

	// SecretBinary carries the bytes it was given.
	{
		impl::sframe::SecretBinary secret(material);
		if (secret.bytes() != material || secret.size() != material.size() || secret.empty()) {
			std::cout << "CleanseTest SecretBinary did not hold its bytes\n";
			cleanseFailures++;
		}

		impl::sframe::SecretBinary fresh;
		if (!fresh.empty() || fresh.size() != 0) {
			std::cout << "CleanseTest a default SecretBinary was not empty\n";
			cleanseFailures++;
		}
	}

	// Assignment replaces the contents rather than merging or clearing them.
	{
		const binary replacement(48, std::byte{0x5A});

		impl::sframe::SecretBinary secret(material);
		secret = replacement;
		if (secret.bytes() != replacement) {
			std::cout << "CleanseTest assigning a binary did not replace the contents\n";
			cleanseFailures++;
		}

		impl::sframe::SecretBinary other(material);
		secret = other;
		if (secret.bytes() != material) {
			std::cout << "CleanseTest copy assignment did not replace the contents\n";
			cleanseFailures++;
		}

		impl::sframe::SecretBinary moved(replacement);
		secret = std::move(moved);
		if (secret.bytes() != replacement) {
			std::cout << "CleanseTest move assignment did not replace the contents\n";
			cleanseFailures++;
		}
	}

	// Assigning to itself must not clear it. Every assignment clears the destination before taking
	// the source, so without the guard against this the key would be wiped and then "taken" from
	// the buffer that had just been zeroed -- leaving zeroes where a live key belongs. Routed
	// through a pointer so the compiler's self-assignment warnings do not fire on a deliberate
	// case.
	{
		impl::sframe::SecretBinary secret(material);
		impl::sframe::SecretBinary *alias = &secret;

		secret = *alias;
		if (secret.bytes() != material) {
			std::cout << "CleanseTest self copy assignment cleared the key\n";
			cleanseFailures++;
		}

		secret = std::move(*alias);
		if (secret.bytes() != material) {
			std::cout << "CleanseTest self move assignment cleared the key\n";
			cleanseFailures++;
		}
	}

	// A moved-from SecretBinary is empty, so its own clearing is a no-op and the bytes travel with
	// the heap block rather than being wiped out from under the new owner.
	{
		impl::sframe::SecretBinary source(material);
		impl::sframe::SecretBinary sink(std::move(source));

		if (sink.bytes() != material) {
			std::cout << "CleanseTest move construction lost the bytes\n";
			cleanseFailures++;
		}
		if (!source.empty()) {
			std::cout << "CleanseTest a moved-from SecretBinary was not empty\n";
			cleanseFailures++;
		}
	}

	try {
		if (cleanseFailures != 0) {
			throw std::runtime_error("Failed SFrame Key Cleansing Tests");
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		throw;
	}
}

// Helper function to check encoding
bool compareEncoded(const binary &a, const binary &b) {
	if (a.size() != b.size())
		return false;

	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i] != b[i])
			return false;
	}
	return true;
}

binary convertFromHex(const string &hex) {
	if (hex.size() % 2 != 0) {
		throw std::invalid_argument("Hex string length must be even)");
	}
	int length = int(hex.length());
	binary result = binary();

	for (int i = 0; i < length; i += 2) {
		string byteString = hex.substr(i, 2);
		uint8_t value = static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16));
		result.push_back(std::byte(value));
	}
	return result;
}

string convertToHex(const binary &bytes) {
	static const char digits[] = "0123456789abcdef";
	string hex;
	hex.reserve(bytes.size() * 2);
	for (std::byte b : bytes) {
		const uint8_t value = std::to_integer<uint8_t>(b);
		hex.push_back(digits[value >> 4]);
		hex.push_back(digits[value & 0x0F]);
	}
	return hex;
}

std::vector<HeaderTestData> loadTestHeaderData() {

	std::vector<HeaderTestData> headerTestVectors;
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(0ULL).Encoded("00"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(1ULL).Encoded("01"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(255ULL).Encoded("08ff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(256ULL).Encoded("090100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(65535ULL).Encoded("09ffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(65536ULL).Encoded("0a010000"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(16777215ULL).Encoded("0affffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(0ULL).Ctr(16777216ULL).Encoded("0b01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(4294967295ULL).Encoded("0bffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(4294967296ULL).Encoded("0c0100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(1099511627775ULL).Encoded("0cffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(1099511627776ULL).Encoded("0d010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(281474976710655ULL).Encoded("0dffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(281474976710656ULL).Encoded("0e01000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(72057594037927935ULL).Encoded("0effffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(72057594037927936ULL).Encoded("0f0100000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(0ULL).Ctr(18446744073709551615ULL).Encoded("0fffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(0ULL).Encoded("10"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(1ULL).Encoded("11"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(255ULL).Encoded("18ff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(256ULL).Encoded("190100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(65535ULL).Encoded("19ffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(65536ULL).Encoded("1a010000"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(16777215ULL).Encoded("1affffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(1ULL).Ctr(16777216ULL).Encoded("1b01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(4294967295ULL).Encoded("1bffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(4294967296ULL).Encoded("1c0100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(1099511627775ULL).Encoded("1cffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(1099511627776ULL).Encoded("1d010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(281474976710655ULL).Encoded("1dffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(281474976710656ULL).Encoded("1e01000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(72057594037927935ULL).Encoded("1effffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(72057594037927936ULL).Encoded("1f0100000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1ULL).Ctr(18446744073709551615ULL).Encoded("1fffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(255ULL).Ctr(0ULL).Encoded("80ff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(255ULL).Ctr(1ULL).Encoded("81ff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(255ULL).Ctr(255ULL).Encoded("88ffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(255ULL).Ctr(256ULL).Encoded("89ff0100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(255ULL).Ctr(65535ULL).Encoded("89ffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(255ULL).Ctr(65536ULL).Encoded("8aff010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(16777215ULL).Encoded("8affffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(16777216ULL).Encoded("8bff01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(4294967295ULL).Encoded("8bffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(4294967296ULL).Encoded("8cff0100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(1099511627775ULL).Encoded("8cffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(1099511627776ULL).Encoded("8dff010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(281474976710655ULL).Encoded("8dffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(281474976710656ULL).Encoded("8eff01000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(72057594037927935ULL).Encoded("8effffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(72057594037927936ULL).Encoded("8fff0100000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(255ULL).Ctr(18446744073709551615ULL).Encoded("8fffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(256ULL).Ctr(0ULL).Encoded("900100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(256ULL).Ctr(1ULL).Encoded("910100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(256ULL).Ctr(255ULL).Encoded("980100ff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(256ULL).Ctr(256ULL).Encoded("9901000100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(256ULL).Ctr(65535ULL).Encoded("990100ffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(256ULL).Ctr(65536ULL).Encoded("9a0100010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(16777215ULL).Encoded("9a0100ffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(16777216ULL).Encoded("9b010001000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(4294967295ULL).Encoded("9b0100ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(4294967296ULL).Encoded("9c01000100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(1099511627775ULL).Encoded("9c0100ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(1099511627776ULL).Encoded("9d0100010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(281474976710655ULL).Encoded("9d0100ffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(281474976710656ULL).Encoded("9e010001000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(72057594037927935ULL).Encoded("9e0100ffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(256ULL).Ctr(72057594037927936ULL).Encoded("9f01000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(256ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("9f0100ffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65535ULL).Ctr(0ULL).Encoded("90ffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65535ULL).Ctr(1ULL).Encoded("91ffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65535ULL).Ctr(255ULL).Encoded("98ffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65535ULL).Ctr(256ULL).Encoded("99ffff0100"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65535ULL).Ctr(65535ULL).Encoded("99ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(65536ULL).Encoded("9affff010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(16777215ULL).Encoded("9affffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(16777216ULL).Encoded("9bffff01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(4294967295ULL).Encoded("9bffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(4294967296ULL).Encoded("9cffff0100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(1099511627775ULL).Encoded("9cffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(1099511627776ULL).Encoded("9dffff010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(281474976710655ULL).Encoded("9dffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(281474976710656ULL).Encoded("9effff01000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(72057594037927935ULL).Encoded("9effffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65535ULL).Ctr(72057594037927936ULL).Encoded("9fffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(65535ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("9fffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65536ULL).Ctr(0ULL).Encoded("a0010000"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65536ULL).Ctr(1ULL).Encoded("a1010000"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65536ULL).Ctr(255ULL).Encoded("a8010000ff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(65536ULL).Ctr(256ULL).Encoded("a90100000100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(65535ULL).Encoded("a9010000ffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(65536ULL).Encoded("aa010000010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(16777215ULL).Encoded("aa010000ffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(16777216ULL).Encoded("ab01000001000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(4294967295ULL).Encoded("ab010000ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(4294967296ULL).Encoded("ac0100000100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(1099511627775ULL).Encoded("ac010000ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(1099511627776ULL).Encoded("ad010000010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(281474976710655ULL).Encoded("ad010000ffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(281474976710656ULL).Encoded("ae01000001000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(65536ULL).Ctr(72057594037927935ULL).Encoded("ae010000ffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(65536ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("af0100000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(65536ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("af010000ffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(16777215ULL).Ctr(0ULL).Encoded("a0ffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(16777215ULL).Ctr(1ULL).Encoded("a1ffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(255ULL).Encoded("a8ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(256ULL).Encoded("a9ffffff0100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(65535ULL).Encoded("a9ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(65536ULL).Encoded("aaffffff010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(16777215ULL).Encoded("aaffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(16777216ULL).Encoded("abffffff01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(4294967295ULL).Encoded("abffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(4294967296ULL).Encoded("acffffff0100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(1099511627775ULL).Encoded("acffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(1099511627776ULL).Encoded("adffffff010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777215ULL).Ctr(281474976710655ULL).Encoded("adffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777215ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("aeffffff01000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777215ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("aeffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777215ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("afffffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777215ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("afffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(16777216ULL).Ctr(0ULL).Encoded("b001000000"));
	headerTestVectors.push_back(HeaderTestData{}.Kid(16777216ULL).Ctr(1ULL).Encoded("b101000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(255ULL).Encoded("b801000000ff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(256ULL).Encoded("b9010000000100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(65535ULL).Encoded("b901000000ffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(65536ULL).Encoded("ba01000000010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(16777215ULL).Encoded("ba01000000ffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(16777216ULL).Encoded("bb0100000001000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(4294967295ULL).Encoded("bb01000000ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(4294967296ULL).Encoded("bc010000000100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(1099511627775ULL).Encoded("bc01000000ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(16777216ULL).Ctr(1099511627776ULL).Encoded("bd01000000010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777216ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("bd01000000ffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777216ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("be0100000001000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777216ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("be01000000ffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777216ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("bf010000000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(16777216ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("bf01000000ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(0ULL).Encoded("b0ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(1ULL).Encoded("b1ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(255ULL).Encoded("b8ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(256ULL).Encoded("b9ffffffff0100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(65535ULL).Encoded("b9ffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(65536ULL).Encoded("baffffffff010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(16777215ULL).Encoded("baffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(16777216ULL).Encoded("bbffffffff01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(4294967295ULL).Encoded("bbffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(4294967296ULL).Encoded("bcffffffff0100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967295ULL).Ctr(1099511627775ULL).Encoded("bcffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967295ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("bdffffffff010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967295ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("bdffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967295ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("beffffffff01000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967295ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("beffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967295ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("bfffffffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967295ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("bfffffffffffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(0ULL).Encoded("c00100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(1ULL).Encoded("c10100000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(255ULL).Encoded("c80100000000ff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(256ULL).Encoded("c901000000000100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(65535ULL).Encoded("c90100000000ffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(65536ULL).Encoded("ca0100000000010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(16777215ULL).Encoded("ca0100000000ffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(16777216ULL).Encoded("cb010000000001000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(4294967295ULL).Encoded("cb0100000000ffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(4294967296ULL).Ctr(4294967296ULL).Encoded("cc01000000000100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("cc0100000000ffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("cd0100000000010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("cd0100000000ffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("ce010000000001000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("ce0100000000ffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("cf01000000000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(4294967296ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("cf0100000000ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(0ULL).Encoded("c0ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(1ULL).Encoded("c1ffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(255ULL).Encoded("c8ffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(256ULL).Encoded("c9ffffffffff0100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(65535ULL).Encoded("c9ffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(65536ULL).Encoded("caffffffffff010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(16777215ULL).Encoded("caffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(16777216ULL).Encoded("cbffffffffff01000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627775ULL).Ctr(4294967295ULL).Encoded("cbffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("ccffffffffff0100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("ccffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("cdffffffffff010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("cdffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("ceffffffffff01000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("ceffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("cfffffffffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627775ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("cfffffffffffffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(0ULL).Encoded("d0010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(1ULL).Encoded("d1010000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(255ULL).Encoded("d8010000000000ff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(256ULL).Encoded("d90100000000000100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(65535ULL).Encoded("d9010000000000ffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(65536ULL).Encoded("da010000000000010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(16777215ULL).Encoded("da010000000000ffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(1099511627776ULL).Ctr(16777216ULL).Encoded("db01000000000001000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(4294967295ULL)
	                                .Encoded("db010000000000ffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("dc0100000000000100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("dc010000000000ffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("dd010000000000010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("dd010000000000ffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("de01000000000001000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("de010000000000ffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("df0100000000000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(1099511627776ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("df010000000000ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(0ULL).Encoded("d0ffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(1ULL).Encoded("d1ffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(255ULL).Encoded("d8ffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(256ULL).Encoded("d9ffffffffffff0100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(65535ULL).Encoded("d9ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(65536ULL).Encoded("daffffffffffff010000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710655ULL).Ctr(16777215ULL).Encoded("daffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(16777216ULL)
	                                .Encoded("dbffffffffffff01000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(4294967295ULL)
	                                .Encoded("dbffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("dcffffffffffff0100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("dcffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("ddffffffffffff010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("ddffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("deffffffffffff01000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("deffffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("dfffffffffffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710655ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("dfffffffffffffffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710656ULL).Ctr(0ULL).Encoded("e001000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710656ULL).Ctr(1ULL).Encoded("e101000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710656ULL).Ctr(255ULL).Encoded("e801000000000000ff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710656ULL).Ctr(256ULL).Encoded("e9010000000000000100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710656ULL).Ctr(65535ULL).Encoded("e901000000000000ffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(281474976710656ULL).Ctr(65536ULL).Encoded("ea01000000000000010000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(16777215ULL)
	                                .Encoded("ea01000000000000ffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(16777216ULL)
	                                .Encoded("eb0100000000000001000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(4294967295ULL)
	                                .Encoded("eb01000000000000ffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("ec010000000000000100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("ec01000000000000ffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("ed01000000000000010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("ed01000000000000ffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("ee0100000000000001000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("ee01000000000000ffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("ef010000000000000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(281474976710656ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("ef01000000000000ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927935ULL).Ctr(0ULL).Encoded("e0ffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927935ULL).Ctr(1ULL).Encoded("e1ffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927935ULL).Ctr(255ULL).Encoded("e8ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927935ULL).Ctr(256ULL).Encoded("e9ffffffffffffff0100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927935ULL).Ctr(65535ULL).Encoded("e9ffffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927935ULL).Ctr(65536ULL).Encoded("eaffffffffffffff010000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(16777215ULL)
	                                .Encoded("eaffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(16777216ULL)
	                                .Encoded("ebffffffffffffff01000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(4294967295ULL)
	                                .Encoded("ebffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("ecffffffffffffff0100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("ecffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("edffffffffffffff010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("edffffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("eeffffffffffffff01000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("eeffffffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("efffffffffffffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927935ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("efffffffffffffffffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927936ULL).Ctr(0ULL).Encoded("f00100000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927936ULL).Ctr(1ULL).Encoded("f10100000000000000"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927936ULL).Ctr(255ULL).Encoded("f80100000000000000ff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927936ULL).Ctr(256ULL).Encoded("f901000000000000000100"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(72057594037927936ULL).Ctr(65535ULL).Encoded("f90100000000000000ffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(65536ULL)
	                                .Encoded("fa0100000000000000010000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(16777215ULL)
	                                .Encoded("fa0100000000000000ffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(16777216ULL)
	                                .Encoded("fb010000000000000001000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(4294967295ULL)
	                                .Encoded("fb0100000000000000ffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("fc01000000000000000100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("fc0100000000000000ffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("fd0100000000000000010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("fd0100000000000000ffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("fe010000000000000001000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("fe0100000000000000ffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("ff01000000000000000100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(72057594037927936ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("ff0100000000000000ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(18446744073709551615ULL).Ctr(0ULL).Encoded("f0ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(18446744073709551615ULL).Ctr(1ULL).Encoded("f1ffffffffffffffff"));
	headerTestVectors.push_back(
	    HeaderTestData{}.Kid(18446744073709551615ULL).Ctr(255ULL).Encoded("f8ffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(256ULL)
	                                .Encoded("f9ffffffffffffffff0100"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(65535ULL)
	                                .Encoded("f9ffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(65536ULL)
	                                .Encoded("faffffffffffffffff010000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(16777215ULL)
	                                .Encoded("faffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(16777216ULL)
	                                .Encoded("fbffffffffffffffff01000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(4294967295ULL)
	                                .Encoded("fbffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(4294967296ULL)
	                                .Encoded("fcffffffffffffffff0100000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(1099511627775ULL)
	                                .Encoded("fcffffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(1099511627776ULL)
	                                .Encoded("fdffffffffffffffff010000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(281474976710655ULL)
	                                .Encoded("fdffffffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(281474976710656ULL)
	                                .Encoded("feffffffffffffffff01000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(72057594037927935ULL)
	                                .Encoded("feffffffffffffffffffffffffffffff"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(72057594037927936ULL)
	                                .Encoded("ffffffffffffffffff0100000000000000"));
	headerTestVectors.push_back(HeaderTestData{}
	                                .Kid(18446744073709551615ULL)
	                                .Ctr(18446744073709551615ULL)
	                                .Encoded("ffffffffffffffffffffffffffffffffff"));

	return headerTestVectors;
}

std::vector<EncryptionTestData> loadTestEncryptionData() {
	std::vector<EncryptionTestData> encryptionTestVectors;

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(1)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230001")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230001")
	        .SFrameSecret("d926952ca8b7ec4a95941d1ada3a5203ceff8cceee34f574d23909eb314c40c0")
	        .SFrameKey("3f7d9a7c83ae8e1c8a11ae695ab59314b367e359fadac7b9c46b2bc6f81f46e16b96f081186"
	                   "8d59402b7e870102720b3")
	        .SFrameSalt("50b29329a04dc0f184ac3168")
	        .Nonce("50b29329a04dc0f184ac740f")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("9901234567449408b6f490086165b9d6f62b24ae1a59a56486b4ae8ed036b88912e24f11"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(2)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230002")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230002")
	        .SFrameSecret("d926952ca8b7ec4a95941d1ada3a5203ceff8cceee34f574d23909eb314c40c0")
	        .SFrameKey("e2ec5c797540310483b16bf6e7a570d2a27d192fe869c7ccd8584a8d9dab91549fbe553f511"
	                   "3461ec6aa83bf3865553e")
	        .SFrameSalt("e68ac8dd3d02fbcd368c5577")
	        .Nonce("e68ac8dd3d02fbcd368c1010")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("99012345673f31438db4d09434e43afa0f8a2f00867a2be085046a9f5cb4f101d607"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(3)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230003")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230003")
	        .SFrameSecret("d926952ca8b7ec4a95941d1ada3a5203ceff8cceee34f574d23909eb314c40c0")
	        .SFrameKey("2c5703089cbb8c583475e4fc461d97d18809df79b6d550f78eb6d50ffa80d89211d57909934"
	                   "f46f5405e38cd583c69fe")
	        .SFrameSalt("38c16e4f5159700c00c7f350")
	        .Nonce("38c16e4f5159700c00c7b637")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("990123456717fc8af28a5a695afcfc6c8df6358a17e26b2fcb3bae32e443"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(4)
	        .Kid(291)
	        .Ctr(17767)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230004")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230004")
	        .SFrameSecret("d926952ca8b7ec4a95941d1ada3a5203ceff8cceee34f574d23909eb314c40c0")
	        .SFrameKey("d34f547f4ca4f9a7447006fe7fcbf768")
	        .SFrameSalt("75234edefe07819026751816")
	        .Nonce("75234edefe07819026755d71")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("9901234567b7412c2513a1b66dbb48841bbaf17f598751176ad847681a69c6d0b091c07018ce4adb34"
	            "eb"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(5)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230005")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230005")
	        .SFrameSecret("0fc3ea6de6aac97a35f194cf9bed94d4b5230f1cb45a785c9fe5dce9c188938ab6ba005b"
	                      "c4c0a19181599e9d1bcf7b74aca48b60bf5e254e546d809313e083a3")
	        .SFrameKey("d3e27b0d4a5ae9e55df01a70e6d4d28d969b246e2936f4b7a5d9b494da6b9633")
	        .SFrameSalt("84991c167b8cd23c93708ec7")
	        .Nonce("84991c167b8cd23c9370cba0")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("990123456794f509d36e9beacb0e261d99c7d1e972f1fed787d4049f17ca21353c1cc24d56ceabced2"
	            "79"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(6)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230006")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230006")
	        .SFrameSecret("0fc3ea6de6aac97a35f194cf9bed94d4b5230f1cb45a785c9fe5dce9c188938ab6ba005b"
	                      "c4c0a19181599e9d1bcf7b74aca48b60bf5e254e546d809313e083a3")
	        .SFrameKey("3c343886ec1c79278836863e00fe934c8894460cfa367ebdc4856b0a9268a4f4fb994378768"
	                   "19394ef90b10ee12602d023f7128ee50f2314c2cc3cff4c56616d2fe03ad2a254cc2ed29b2a"
	                   "4d3f2534c0dda9e7c391ad1917ea07aa221dd4b224")
	        .SFrameSalt("e082f7ce012ad30c87c49e3f")
	        .Nonce("e082f7ce012ad30c87c4db58")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("9901234567b369e03ec6467ad505ddc84914115069280c5c797555be6e32cde6ac25bc9e"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(7)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230007")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230007")
	        .SFrameSecret("0fc3ea6de6aac97a35f194cf9bed94d4b5230f1cb45a785c9fe5dce9c188938ab6ba005b"
	                      "c4c0a19181599e9d1bcf7b74aca48b60bf5e254e546d809313e083a3")
	        .SFrameKey("7271d6c6cbccd2e2343d480ebea65718a7bb379eefcf3f8d107c1e2a76e755293a497fd9e4e"
	                   "8291b965161987ef4ef24983eabb06cb0a392defaab18654780a39c106ffa4a47d4183a6e59"
	                   "3cd0c1bcab2b9c6dcf049215845bfb7580c4dea80e")
	        .SFrameSalt("46b4367993a314910d4d9f3d")
	        .Nonce("46b4367993a314910d4dda5a")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("990123456797cb5644d8831ff8bdc080249990b24b569144cab2a87be22c20d97976"));

	encryptionTestVectors.push_back(
	    EncryptionTestData{}
	        .CipherSuite(8)
	        .Kid(291ULL)
	        .Ctr(17767ULL)
	        .BaseKey("000102030405060708090a0b0c0d0e0f")
	        .SFrameKeyLabel("534672616d6520312e3020536563726574206b65792000000000000001230008")
	        .SFrameSaltLabel("534672616d6520312e30205365637265742073616c742000000000000001230008")
	        .SFrameSecret("0fc3ea6de6aac97a35f194cf9bed94d4b5230f1cb45a785c9fe5dce9c188938ab6ba005b"
	                      "c4c0a19181599e9d1bcf7b74aca48b60bf5e254e546d809313e083a3")
	        .SFrameKey("afe92c81e0df8c00fab619e0559fe5aeefce1ef77789d4c728af1b1c1f2e3552c405d274415"
	                   "a5291ec075c2d9954c450fbd36682a4e978494808b703ce78b409f9fec29b91e6e703a75c41"
	                   "31377c80c9d51b8906088092452e2593eb142eea2d")
	        .SFrameSalt("f6de647bac1263524cfb6533")
	        .Nonce("f6de647bac1263524cfb2054")
	        .Metadata("4945544620534672616d65205747")
	        .Aad("99012345674945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("9901234567112a94a288b85b49ffef1d279f2830165c39d76cac8884011c"));
	return encryptionTestVectors;
}

std::vector<CTRHMACTestData> loadTestHMACCTRData() {
	std::vector<CTRHMACTestData> encryptionTestVectors;

	encryptionTestVectors.push_back(
	    CTRHMACTestData{}
	        .CipherSuite(1)
	        .Key("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223242526272"
	             "8292a2b2c2d2e2f")
	        .EncKey("000102030405060708090a0b0c0d0e0f")
	        .AuthKey("101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f")
	        .Nonce("101112131415161718191a1b")
	        .Aad("4945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("6339af04ada1d064688a442b8dc69d5b6bfa40f4bef0583e8081069cc60705"));

	encryptionTestVectors.push_back(
	    CTRHMACTestData{}
	        .CipherSuite(2)
	        .Key("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223242526272"
	             "8292a2b2c2d2e2f")
	        .EncKey("000102030405060708090a0b0c0d0e0f")
	        .AuthKey("101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f")
	        .Nonce("101112131415161718191a1b")
	        .Aad("4945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("6339af04ada1d064688a442b8dc69d5b6bfa40f4be6e93b7da076927bb"));

	encryptionTestVectors.push_back(
	    CTRHMACTestData{}
	        .CipherSuite(3)
	        .Key("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223242526272"
	             "8292a2b2c2d2e2f")
	        .EncKey("000102030405060708090a0b0c0d0e0f")
	        .AuthKey("101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f")
	        .Nonce("101112131415161718191a1b")
	        .Aad("4945544620534672616d65205747")
	        .Pt("64726166742d696574662d736672616d652d656e63")
	        .Ct("6339af04ada1d064688a442b8dc69d5b6bfa40f4be09480509"));

	return encryptionTestVectors;
}

#endif // RTC_ENABLE_MEDIA
