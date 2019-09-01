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

#include <gnutls/crypto.h>

using std::shared_ptr;
using std::string;

namespace {

void check_gnutls(int ret, const string &message = "GnuTLS error") {
	if (ret != GNUTLS_E_SUCCESS)
		throw std::runtime_error(message + ": " + gnutls_strerror(ret));
}

gnutls_certificate_credentials_t *create_credentials() {
	auto creds = new gnutls_certificate_credentials_t;
	check_gnutls(gnutls_certificate_allocate_credentials(creds));
	return creds;
}

void delete_credentials(gnutls_certificate_credentials_t *creds) {
	gnutls_certificate_free_credentials(*creds);
	delete creds;
}

gnutls_x509_crt_t *create_crt() {
	auto crt = new gnutls_x509_crt_t;
	check_gnutls(gnutls_x509_crt_init(crt));
	return crt;
}

void delete_crt(gnutls_x509_crt_t *crt) {
	gnutls_x509_crt_deinit(*crt);
	delete crt;
}

gnutls_x509_privkey_t *create_privkey() {
	auto privkey = new gnutls_x509_privkey_t;
	check_gnutls(gnutls_x509_privkey_init(privkey));
	return privkey;
}

void delete_privkey(gnutls_x509_privkey_t *privkey) {
	gnutls_x509_privkey_deinit(*privkey);
	delete privkey;
}

gnutls_datum_t make_datum(char *data, size_t size) {
	gnutls_datum_t datum;
	datum.data = reinterpret_cast<unsigned char *>(data);
	datum.size = size;
	return datum;
}

} // namespace

namespace rtc {

Certificate::Certificate(string crt_pem, string key_pem)
    : mCredentials(create_credentials(), delete_credentials) {

	gnutls_datum_t crt_datum = make_datum(crt_pem.data(), crt_pem.size());
	gnutls_datum_t key_datum = make_datum(key_pem.data(), key_pem.size());

	check_gnutls(gnutls_certificate_set_x509_key_mem(*mCredentials, &crt_datum, &key_datum,
	                                                 GNUTLS_X509_FMT_PEM),
	             "Unable to import PEM");

	auto create_crt_list = [this]() -> gnutls_x509_crt_t * {
		gnutls_x509_crt_t *crt_list = nullptr;
		unsigned int crt_list_size = 0;
		check_gnutls(gnutls_certificate_get_x509_crt(*mCredentials, 0, &crt_list, &crt_list_size));
		assert(crt_list_size == 1);
		return crt_list;
	};

	auto delete_crt_list = [](gnutls_x509_crt_t *crt_list) {
		gnutls_x509_crt_deinit(crt_list[0]);
		gnutls_free(crt_list);
	};

	std::unique_ptr<gnutls_x509_crt_t, decltype(delete_crt_list)> crt_list(create_crt_list(),
	                                                                       delete_crt_list);

	mFingerprint = make_fingerprint(*crt_list);
}

Certificate::Certificate(gnutls_x509_crt_t crt, gnutls_x509_privkey_t privkey)
    : mCredentials(create_credentials(), delete_credentials), mFingerprint(make_fingerprint(crt)) {

	check_gnutls(gnutls_certificate_set_x509_key(*mCredentials, &crt, 1, privkey),
	             "Unable to set certificate and key pair in credentials");
}

string Certificate::fingerprint() const { return mFingerprint; }

gnutls_certificate_credentials_t Certificate::credentials() const { return *mCredentials; }

string make_fingerprint(gnutls_x509_crt_t crt) {
	const size_t size = 32;
	unsigned char buffer[size];
	size_t len = size;
	check_gnutls(gnutls_x509_crt_get_fingerprint(crt, GNUTLS_DIG_SHA256, buffer, &len),
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

	std::lock_guard<std::mutex> lock(cacheMutex);
	if (auto it = cache.find(commonName); it != cache.end())
		return it->second;

	std::unique_ptr<gnutls_x509_crt_t, decltype(&delete_crt)> crt(create_crt(), delete_crt);
	std::unique_ptr<gnutls_x509_privkey_t, decltype(&delete_privkey)> privkey(create_privkey(),
	                                                                          delete_privkey);

	const unsigned int bits = gnutls_sec_param_to_pk_bits(GNUTLS_PK_RSA, GNUTLS_SEC_PARAM_HIGH);
	check_gnutls(gnutls_x509_privkey_generate(*privkey, GNUTLS_PK_RSA, bits, 0),
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

	check_gnutls(gnutls_x509_crt_sign2(*crt, *crt, *privkey, GNUTLS_DIG_SHA256, 0),
	             "Unable to auto-sign certificate");

	auto certificate = std::make_shared<Certificate>(*crt, *privkey);
	cache.emplace(std::make_pair(commonName, certificate));
	return certificate;
}

} // namespace rtc
