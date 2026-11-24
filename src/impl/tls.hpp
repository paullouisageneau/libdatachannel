/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_TLS_H
#define RTC_TLS_H

#include "common.hpp"

#include <chrono>

#if USE_GNUTLS

#include <gnutls/gnutls.h>

#include <gnutls/crypto.h>
#include <gnutls/dtls.h>
#include <gnutls/x509.h>

namespace rtc::gnutls {

bool check(int ret, const string &message = "GnuTLS error");

gnutls_certificate_credentials_t *new_credentials();
void free_credentials(gnutls_certificate_credentials_t *creds);

gnutls_x509_crt_t *new_crt();
void free_crt(gnutls_x509_crt_t *crt);

gnutls_x509_privkey_t *new_privkey();
void free_privkey(gnutls_x509_privkey_t *privkey);

gnutls_datum_t make_datum(char *data, size_t size);

} // namespace rtc::gnutls

#elif USE_MBEDTLS

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

// Mbed TLS version 3, relies on internal states when build with MBEDTLS_USE_PSA_CRYPTO.
// Mbed TLS version 4 always realies on internal states.
// In those scenarios, MBEDTLS_THREADING_C is required when using any Mbed TLS function
// unless all calls comes from the same thread.
#if !defined(MBEDTLS_THREADING_C) && (MBEDTLS_VERSION_MAJOR >= 4 || defined(MBEDTLS_USE_PSA_CRYPTO))
#error "Mbed TLS was not build with threading support (MBEDTLS_THREADING_C)."
#endif

namespace rtc::mbedtls {

void init();

int random_func(void *rng, unsigned char *out, size_t len);

int safe_psa(std::function<int()> func);

bool check(int ret, const string &message = "MbedTLS error");

string format_time(const std::chrono::system_clock::time_point &tp);

std::shared_ptr<mbedtls_pk_context> new_pk_context();
std::shared_ptr<mbedtls_x509_crt> new_x509_crt();

} // namespace rtc::mbedtls

#elif USE_SCHANNEL

// Native Windows backend: SChannel via SSPI for TLS/DTLS, CryptoAPI and CNG for certificates,
// keys, and hashes. DTLS 1.2 and keying material export require Windows 10 or later.

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS // for SCH_CREDENTIALS
#endif

// Include winsock2.h first since windows.h may include winsock.h
#include <winsock2.h>

#include <windows.h>

#include <subauth.h> // for UNICODE_STRING, required by SCH_CREDENTIALS

#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>

#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <vector>

namespace rtc::schannel {

string error_string(long code);

// Throw if status is a failure
void check(SECURITY_STATUS status, const string &message = "SChannel error");

std::wstring to_wstring(const string &str);

enum class HashAlgorithm { Sha1, Sha224, Sha256, Sha384, Sha512 };
binary hash(HashAlgorithm algorithm, const void *data, size_t size);

using cert_context_ptr = shared_ptr<const CERT_CONTEXT>;
cert_context_ptr make_cert_context_ptr(PCCERT_CONTEXT ctx); // takes ownership

using cert_store_ptr = shared_ptr<void>; // HCERTSTORE

struct PemBlock {
	string label;
	binary der;
	bool encrypted; // legacy OpenSSL encryption with "Proc-Type: 4,ENCRYPTED" header
};

std::vector<PemBlock> parse_pem(const string &pem);
string read_file(const string &filename);

// Load PEM certificates in a new memory store, the first one is returned in leaf if not null
cert_store_ptr load_pem_certificates(const string &pem, cert_context_ptr *leaf = nullptr);

// SChannel needs to access private keys from the LSA process, so in-memory (ephemeral) CNG keys
// are rejected. PrivateKey therefore wraps a key persisted in the user key store of the
// Microsoft Software Key Storage Provider under a unique name, which is deleted on destruction.
// Keys left behind by crashed processes are cleaned up when a new key is created.
class PrivateKey {
public:
	static shared_ptr<PrivateKey> Generate(bool rsa);
	static shared_ptr<PrivateKey> ImportPkcs8(const binary &der, const string &pass = "");

	PrivateKey(NCRYPT_KEY_HANDLE handle, std::wstring name);
	~PrivateKey();

	PrivateKey(const PrivateKey &) = delete;
	PrivateKey &operator=(const PrivateKey &) = delete;

	NCRYPT_KEY_HANDLE handle() const { return mHandle; }
	const std::wstring &name() const { return mName; }

private:
	static std::wstring NewName();

	NCRYPT_KEY_HANDLE mHandle;
	const std::wstring mName;
};

// Owned SSPI handles
class Credentials {
public:
	Credentials() { SecInvalidateHandle(&mHandle); }
	~Credentials();

	Credentials(const Credentials &) = delete;
	Credentials &operator=(const Credentials &) = delete;

	CredHandle *get() { return &mHandle; }

private:
	CredHandle mHandle;
};

class Context {
public:
	Context() { SecInvalidateHandle(&mHandle); }
	~Context();

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;

	CtxtHandle *get() { return &mHandle; }
	CtxtHandle *getOrNull() { return SecIsValidHandle(&mHandle) ? &mHandle : nullptr; }
	bool valid() const { return SecIsValidHandle(&mHandle); }

private:
	CtxtHandle mHandle;
};

// Buffers allocated by SSPI with ISC_REQ_ALLOCATE_MEMORY
class ContextBuffers {
public:
	ContextBuffers(std::initializer_list<unsigned long> types);
	~ContextBuffers();

	ContextBuffers(const ContextBuffers &) = delete;
	ContextBuffers &operator=(const ContextBuffers &) = delete;

	SecBufferDesc *desc() { return &mDesc; }
	SecBuffer &operator[](size_t i) { return mBuffers.at(i); }
	const SecBuffer *find(unsigned long type) const;

private:
	std::vector<SecBuffer> mBuffers;
	SecBufferDesc mDesc;
};

} // namespace rtc::schannel

#else // OPENSSL

#ifdef _WIN32
// Include winsock2.h header first since OpenSSL may include winsock.h
#include <winsock2.h>
#endif

#include <openssl/ssl.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#ifndef BIO_EOF
#define BIO_EOF -1
#endif

namespace rtc::openssl {

void init();
string error_string(unsigned long error);

bool check(int success, const string &message = "OpenSSL error");
bool check_error(int err, const string &message = "OpenSSL error",
                 const SSL *ssl = nullptr);

BIO *BIO_new_from_file(const string &filename);

void SSL_CTX_add_cert_to_store_from_pem(SSL_CTX *ctx, const string &pem);

} // namespace rtc::openssl

#endif

#endif
