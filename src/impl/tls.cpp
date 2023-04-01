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

#elif USE_MBEDTLS

#include <time.h>

namespace {

// Safe gmtime
int my_gmtime(const time_t *t, struct tm *buf) {
#ifdef _WIN32
	return ::gmtime_s(buf, t) == 0 ? 0 : -1;
#else // POSIX
	return ::gmtime_r(t, buf) != NULL ? 0 : -1;
#endif
}

// Format time_t as UTC
size_t my_strftme(char *buf, size_t size, const char *format, const time_t *t) {
	struct tm g;
	if (my_gmtime(t, &g) != 0)
		return 0;

	return ::strftime(buf, size, format, &g);
}

} // namespace

namespace rtc::mbedtls {

void check(int ret, const string &message) {
	if (ret < 0) {
		const size_t bufferSize = 1024;
		char buffer[bufferSize];
		mbedtls_strerror(ret, reinterpret_cast<char *>(buffer), bufferSize);
		PLOG_ERROR << message << ": " << buffer;
		throw std::runtime_error(message + ": " + std::string(buffer));
	}
}

string format_time(const std::chrono::system_clock::time_point &tp) {
	time_t t = std::chrono::system_clock::to_time_t(tp);
	const size_t bufferSize = 256;
	char buffer[bufferSize];
	if (my_strftme(buffer, bufferSize, "%Y%m%d%H%M%S", &t) == 0)
		throw std::runtime_error("Time conversion failed");

	return string(buffer);
};

std::shared_ptr<mbedtls_pk_context> new_pk_context() {
	return std::shared_ptr<mbedtls_pk_context>{[]() {
		                                           auto p = new mbedtls_pk_context;
		                                           mbedtls_pk_init(p);
		                                           return p;
	                                           }(),
	                                           [](mbedtls_pk_context *p) {
		                                           mbedtls_pk_free(p);
		                                           delete p;
	                                           }};
}

std::shared_ptr<mbedtls_x509_crt> new_x509_crt() {
	return std::shared_ptr<mbedtls_x509_crt>{[]() {
		                                         auto p = new mbedtls_x509_crt;
		                                         mbedtls_x509_crt_init(p);
		                                         return p;
	                                         }(),
	                                         [](mbedtls_x509_crt *crt) {
		                                         mbedtls_x509_crt_free(crt);
		                                         delete crt;
	                                         }};
}

} // namespace rtc::mbedtls

#else // OPENSSL

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

string error_string(unsigned long err) {
	const size_t bufferSize = 256;
	char buffer[bufferSize];
	ERR_error_string_n(err, buffer, bufferSize);
	return string(buffer);
}

bool check(int success, const string &message) {
	if (success)
		return true;

	string str = error_string(ERR_get_error());
	PLOG_ERROR << message << ": " << str;
	throw std::runtime_error(message + ": " + str);
}

bool check(SSL *ssl, int ret, const string &message) {
	if (ret == BIO_EOF)
		return true;

	unsigned long err = SSL_get_error(ssl, ret);
	if (err == SSL_ERROR_NONE || err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		return true;
	}
	if (err == SSL_ERROR_ZERO_RETURN) {
		PLOG_DEBUG << "DTLS connection cleanly closed";
		return false;
	}
	string str = error_string(err);
	PLOG_ERROR << str;
	throw std::runtime_error(message + ": " + str);
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
