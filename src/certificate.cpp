/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "certificate.hpp"

#include <cassert>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

using std::shared_ptr;
using std::string;
using std::unique_ptr;

#if USE_GNUTLS

namespace rtc {

Certificate::Certificate(string crt_pem, string key_pem)
    : mCredentials(gnutls::new_credentials(), gnutls::free_credentials) {

	gnutls_datum_t crt_datum = gnutls::make_datum(crt_pem.data(), crt_pem.size());
	gnutls_datum_t key_datum = gnutls::make_datum(key_pem.data(), key_pem.size());

	gnutls::check(gnutls_certificate_set_x509_key_mem(*mCredentials, &crt_datum, &key_datum,
	                                                  GNUTLS_X509_FMT_PEM),
	              "Unable to import PEM");

	auto new_crt_list = [this]() -> gnutls_x509_crt_t * {
		gnutls_x509_crt_t *crt_list = nullptr;
		unsigned int crt_list_size = 0;
		gnutls::check(gnutls_certificate_get_x509_crt(*mCredentials, 0, &crt_list, &crt_list_size));
		assert(crt_list_size == 1);
		return crt_list;
	};

	auto free_crt_list = [](gnutls_x509_crt_t *crt_list) {
		gnutls_x509_crt_deinit(crt_list[0]);
		gnutls_free(crt_list);
	};

	std::unique_ptr<gnutls_x509_crt_t, decltype(free_crt_list)> crt_list(new_crt_list(),
	                                                                     free_crt_list);

	mFingerprint = make_fingerprint(*crt_list);
}

Certificate::Certificate(gnutls_x509_crt_t crt, gnutls_x509_privkey_t privkey)
    : mCredentials(gnutls::new_credentials(), gnutls::free_credentials),
      mFingerprint(make_fingerprint(crt)) {

	gnutls::check(gnutls_certificate_set_x509_key(*mCredentials, &crt, 1, privkey),
	              "Unable to set certificate and key pair in credentials");
}

gnutls_certificate_credentials_t Certificate::credentials() const { return *mCredentials; }

string Certificate::fingerprint() const { return mFingerprint; }

string make_fingerprint(gnutls_x509_crt_t crt) {
	const size_t size = 32;
	unsigned char buffer[size];
	size_t len = size;
	gnutls::check(gnutls_x509_crt_get_fingerprint(crt, GNUTLS_DIG_SHA256, buffer, &len),
	              "X509 fingerprint error");

	std::ostringstream oss;
	oss << std::hex << std::uppercase << std::setfill('0');
	for (size_t i = 0; i < len; ++i) {
		if (i)
			oss << std::setw(1) << ':';
		oss << std::setw(2) << unsigned(buffer[i]);
	}
	return oss.str();
}

shared_ptr<Certificate> make_certificate(const string &commonName) {
	static std::unordered_map<string, shared_ptr<Certificate>> cache;
	static std::mutex cacheMutex;

	std::lock_guard lock(cacheMutex);
	if (auto it = cache.find(commonName); it != cache.end())
		return it->second;

	using namespace gnutls;
	unique_ptr<gnutls_x509_crt_t, decltype(&free_crt)> crt(new_crt(), free_crt);
	unique_ptr<gnutls_x509_privkey_t, decltype(&free_privkey)> privkey(new_privkey(), free_privkey);

	const unsigned int bits = gnutls_sec_param_to_pk_bits(GNUTLS_PK_RSA, GNUTLS_SEC_PARAM_HIGH);
	gnutls::check(gnutls_x509_privkey_generate(*privkey, GNUTLS_PK_RSA, bits, 0),
	              "Unable to generate key pair");

	using namespace std::chrono;
	auto now = time_point_cast<seconds>(system_clock::now());
	gnutls_x509_crt_set_activation_time(*crt, (now - hours(1)).time_since_epoch().count());
	gnutls_x509_crt_set_expiration_time(*crt, (now + hours(24 * 365)).time_since_epoch().count());
	gnutls_x509_crt_set_version(*crt, 1);
	gnutls_x509_crt_set_key(*crt, *privkey);
	gnutls_x509_crt_set_dn_by_oid(*crt, GNUTLS_OID_X520_COMMON_NAME, 0, commonName.data(),
	                              commonName.size());

	const size_t serialSize = 16;
	char serial[serialSize];
	gnutls_rnd(GNUTLS_RND_NONCE, serial, serialSize);
	gnutls_x509_crt_set_serial(*crt, serial, serialSize);

	gnutls::check(gnutls_x509_crt_sign2(*crt, *crt, *privkey, GNUTLS_DIG_SHA256, 0),
	              "Unable to auto-sign certificate");

	auto certificate = std::make_shared<Certificate>(*crt, *privkey);
	cache.emplace(std::make_pair(commonName, certificate));
	return certificate;
}

} // namespace rtc

#else // USE_GNUTLS==0

namespace rtc {

Certificate::Certificate(string crt_pem, string key_pem) {
	BIO *bio = BIO_new(BIO_s_mem());
	BIO_write(bio, crt_pem.data(), crt_pem.size());
	mX509 = shared_ptr<X509>(PEM_read_bio_X509(bio, nullptr, 0, 0), X509_free);
	BIO_free(bio);
	if (!mX509)
		throw std::invalid_argument("Unable to import certificate PEM");

	bio = BIO_new(BIO_s_mem());
	BIO_write(bio, key_pem.data(), key_pem.size());
	mPKey = shared_ptr<EVP_PKEY>(PEM_read_bio_PrivateKey(bio, nullptr, 0, 0), EVP_PKEY_free);
	BIO_free(bio);
	if (!mPKey)
		throw std::invalid_argument("Unable to import PEM key PEM");

	mFingerprint = make_fingerprint(mX509.get());
}

Certificate::Certificate(shared_ptr<X509> x509, shared_ptr<EVP_PKEY> pkey) :
	mX509(std::move(x509)), mPKey(std::move(pkey))
{
	mFingerprint = make_fingerprint(mX509.get());
}

string Certificate::fingerprint() const { return mFingerprint; }

std::tuple<X509 *, EVP_PKEY *> Certificate::credentials() const { return {mX509.get(), mPKey.get()}; }

string make_fingerprint(X509 *x509) {
	const size_t size = 32;
    unsigned char buffer[size];
    unsigned int len = size;
    if (!X509_digest(x509, EVP_sha256(), buffer, &len))
      throw std::runtime_error("X509 fingerprint error");

	std::ostringstream oss;
	oss << std::hex << std::uppercase << std::setfill('0');
	for (size_t i = 0; i < len; ++i) {
		if (i)
			oss << std::setw(1) << ':';
		oss << std::setw(2) << unsigned(buffer[i]);
	}
	return oss.str();
}


shared_ptr<Certificate> make_certificate(const string &commonName) {
	static std::unordered_map<string, shared_ptr<Certificate>> cache;
	static std::mutex cacheMutex;

	std::lock_guard lock(cacheMutex);
	if (auto it = cache.find(commonName); it != cache.end())
		return it->second;

	shared_ptr<X509> x509(X509_new(), X509_free);
	shared_ptr<EVP_PKEY> pkey(EVP_PKEY_new(), EVP_PKEY_free);

    unique_ptr<RSA, decltype(&RSA_free)> rsa(RSA_new(), RSA_free);
	unique_ptr<BIGNUM, decltype(&BN_free)> exponent(BN_new(), BN_free);
	unique_ptr<BIGNUM, decltype(&BN_free)> serial_number(BN_new(), BN_free);
    unique_ptr<X509_NAME, decltype(&X509_NAME_free)> name(X509_NAME_new(), X509_NAME_free);

	if (!x509 || !pkey || !rsa || !exponent || !serial_number || !name)
		throw std::runtime_error("Unable allocate structures for certificate generation");

	const int bits = 4096;
	const unsigned int e = 65537; // 2^16 + 1

	if (!pkey || !rsa || !exponent || !BN_set_word(exponent.get(), e) ||
	    !RSA_generate_key_ex(rsa.get(), bits, exponent.get(), NULL) ||
	    !EVP_PKEY_assign_RSA(pkey.get(), rsa.release())) // the key will be freed when pkey is freed
		throw std::runtime_error("Unable to generate key pair");

	const size_t serialSize = 16;
	const auto *commonNameBytes = reinterpret_cast<const unsigned char *>(commonName.c_str());

	if (!X509_gmtime_adj(X509_get_notBefore(x509.get()), 3600 * -1) ||
	    !X509_gmtime_adj(X509_get_notAfter(x509.get()), 3600 * 24 * 365) ||
	    !X509_set_version(x509.get(), 1) || !X509_set_pubkey(x509.get(), pkey.get()) ||
	    !BN_pseudo_rand(serial_number.get(), serialSize, 0, 0) ||
	    !BN_to_ASN1_INTEGER(serial_number.get(), X509_get_serialNumber(x509.get())) ||
	    !X509_NAME_add_entry_by_NID(name.get(), NID_commonName, MBSTRING_UTF8, commonNameBytes, -1,
	                                -1, 0) ||
	    !X509_set_subject_name(x509.get(), name.get()) ||
	    !X509_set_issuer_name(x509.get(), name.get()))
		throw std::runtime_error("Unable to set certificate properties");

	if (!X509_sign(x509.get(), pkey.get(), EVP_sha256()))
		throw std::runtime_error("Unable to auto-sign certificate");

	auto certificate = std::make_shared<Certificate>(x509, pkey);
	cache.emplace(std::make_pair(commonName, certificate));
	return certificate;
}

} // namespace rtc

#endif

