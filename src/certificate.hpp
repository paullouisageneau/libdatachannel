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

#ifndef RTC_CERTIFICATE_H
#define RTC_CERTIFICATE_H

#include "include.hpp"
#include "tls.hpp"

#include <tuple>

namespace rtc {

class Certificate {
public:
	Certificate(string crt_pem, string key_pem);

#if USE_GNUTLS
	Certificate(gnutls_x509_crt_t crt, gnutls_x509_privkey_t privkey);
	gnutls_certificate_credentials_t credentials() const;
#else
	Certificate(std::shared_ptr<X509> x509, std::shared_ptr<EVP_PKEY> pkey);
	std::tuple<X509 *, EVP_PKEY *> credentials() const;
#endif

	string fingerprint() const;

private:
#if USE_GNUTLS
	std::shared_ptr<gnutls_certificate_credentials_t> mCredentials;
#else
	std::shared_ptr<X509> mX509;
	std::shared_ptr<EVP_PKEY> mPKey;
#endif

	string mFingerprint;
};

#if USE_GNUTLS
string make_fingerprint(gnutls_x509_crt_t crt);
#else
string make_fingerprint(X509 *x509);
#endif

std::shared_ptr<Certificate> make_certificate(const string &commonName);

} // namespace rtc

#endif
