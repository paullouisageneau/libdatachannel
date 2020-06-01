/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
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

#include "tls.hpp"

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
	if (!done) {
		OPENSSL_init_ssl(0, NULL);
		SSL_load_error_strings();
		ERR_load_crypto_strings();
		done = true;
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

} // namespace rtc::openssl

#endif

