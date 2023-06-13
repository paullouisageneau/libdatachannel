/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tls.hpp"

#include "internals.hpp"

#include <fstream>

#if USE_GNUTLS

namespace rtc::gnutls {

bool check(int ret, const string &message) {
	if (ret < 0) {
		if (!gnutls_error_is_fatal(ret)) {
			PLOG_INFO << gnutls_strerror(ret);
			return false;
		}
		PLOG_ERROR << message << ": " << gnutls_strerror(ret);
		throw std::runtime_error(message + ": " + gnutls_strerror(ret));
	}
	return true;
}

gnutls_certificate_credentials_t *new_credentials() {
	auto creds = new gnutls_certificate_credentials_t;
	gnutls::check(gnutls_certificate_allocate_credentials(creds));
	return creds;
}

void free_credentials(gnutls_certificate_credentials_t *creds) {
	gnutls_certificate_free_credentials(*creds);
	delete creds;
}

gnutls_x509_crt_t *new_crt() {
	auto crt = new gnutls_x509_crt_t;
	gnutls::check(gnutls_x509_crt_init(crt));
	return crt;
}

void free_crt(gnutls_x509_crt_t *crt) {
	gnutls_x509_crt_deinit(*crt);
	delete crt;
}

gnutls_x509_privkey_t *new_privkey() {
	auto privkey = new gnutls_x509_privkey_t;
	gnutls::check(gnutls_x509_privkey_init(privkey));
	return privkey;
}

void free_privkey(gnutls_x509_privkey_t *privkey) {
	gnutls_x509_privkey_deinit(*privkey);
	delete privkey;
}

gnutls_datum_t make_datum(char *data, size_t size) {
	gnutls_datum_t datum;
	datum.data = reinterpret_cast<unsigned char *>(data);
	datum.size = size;
	return datum;
}

} // namespace rtc::gnutls

#else // USE_GNUTLS==0

namespace rtc::openssl {

void init() {
	static std::mutex mutex;
	static bool done = false;

	std::lock_guard lock(mutex);
	if (!std::exchange(done, true)) {
		OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
		OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
	}
}

string error_string(unsigned long error) {
	const size_t bufferSize = 256;
	char buffer[bufferSize];
	ERR_error_string_n(error, buffer, bufferSize);
	return string(buffer);
}

bool check(int success, const string &message) {
	unsigned long last_error = ERR_peek_last_error();
	ERR_clear_error();

	if (success > 0)
		return true;

	string str = message;
	if (last_error != 0)
		str += ": " + error_string(last_error);

	PLOG_ERROR << str;
	throw std::runtime_error(str);
}

bool check(SSL *ssl, int ret, const string &message) {
	unsigned long last_error = ERR_peek_last_error();
	ERR_clear_error();

	int err = SSL_get_error(ssl, ret);
	if (err == SSL_ERROR_NONE || err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		return true;
	}
	if (err == SSL_ERROR_ZERO_RETURN) {
		PLOG_DEBUG << "OpenSSL connection cleanly closed";
		return false;
	}

	string str = message;
	if (err == SSL_ERROR_SYSCALL) {
		str += ": fatal I/O error";
	} else if (err == SSL_ERROR_SSL) {
		if (last_error != 0)
			str += ": " + error_string(last_error);
	}
	PLOG_ERROR << str;
	throw std::runtime_error(str);
}

BIO *BIO_new_from_file(const string &filename) {
	BIO *bio = nullptr;
	try {
		std::ifstream ifs(filename, std::ifstream::in | std::ifstream::binary);
		if (!ifs.is_open())
			return nullptr;

		bio = BIO_new(BIO_s_mem());

		const size_t bufferSize = 4096;
		char buffer[bufferSize];
		while (ifs.good()) {
			ifs.read(buffer, bufferSize);
			BIO_write(bio, buffer, int(ifs.gcount()));
		}
		ifs.close();
		return bio;

	} catch (const std::exception &) {
		BIO_free(bio);
		return nullptr;
	}
}

} // namespace rtc::openssl

#endif
