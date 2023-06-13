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

#else // USE_GNUTLS==0

#ifdef _WIN32
// Include winsock2.h header first since OpenSSL may include winsock.h
#include <winsock2.h>
#endif

#ifndef OPENSSL_API_COMPAT
#define OPENSSL_API_COMPAT 0x10100000L
#endif

#include <openssl/ssl.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#ifndef BIO_EOF
#define BIO_EOF -1
#endif

namespace rtc::openssl {

void init();
string error_string(unsigned long error);

bool check(int success, const string &message = "OpenSSL error");
bool check(SSL *ssl, int ret, const string &message = "OpenSSL error");

BIO *BIO_new_from_file(const string &filename);

} // namespace rtc::openssl

#endif

#endif
