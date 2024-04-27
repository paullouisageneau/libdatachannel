/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "verifiedtlstransport.hpp"
#include "common.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

static const string PemBeginCertificateTag = "-----BEGIN CERTIFICATE-----";

VerifiedTlsTransport::VerifiedTlsTransport(
    variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>> lower, string host,
    certificate_ptr certificate, state_callback callback, [[maybe_unused]] optional<string> cacert)
    : TlsTransport(std::move(lower), std::move(host), std::move(certificate), std::move(callback)) {

	PLOG_DEBUG << "Setting up TLS certificate verification";

#if USE_GNUTLS
	gnutls_session_set_verify_cert(mSession, mHost->c_str(), 0);
#elif USE_MBEDTLS
	mbedtls_ssl_conf_authmode(&mConf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_x509_crt_init(&mCaCert);
	try {
		if (cacert) {
			if (cacert->find(PemBeginCertificateTag) == string::npos) {
				// *cacert is a file path
				mbedtls::check(mbedtls_x509_crt_parse_file(&mCaCert, cacert->c_str()));
			} else {
				// *cacert is a PEM content
				mbedtls::check(mbedtls_x509_crt_parse(
				    &mCaCert, reinterpret_cast<const unsigned char *>(cacert->c_str()),
				    cacert->size() + 1));
			}
			mbedtls_ssl_conf_ca_chain(&mConf, &mCaCert, NULL);
		}
	} catch (...) {
		mbedtls_x509_crt_free(&mCaCert);
		throw;
	}
#else
	if (cacert) {
		if (cacert->find(PemBeginCertificateTag) == string::npos) {
			// *cacert is a file path
			openssl::check(SSL_CTX_load_verify_locations(mCtx, cacert->c_str(), NULL), "Failed to load CA certificate");
		} else {
			// *cacert is a PEM content
			PLOG_WARNING << "CA certificate as PEM is not supported for OpenSSL";
		}
	}
	SSL_set_verify(mSsl, SSL_VERIFY_PEER, NULL);
	SSL_set_verify_depth(mSsl, 4);
#endif
}

VerifiedTlsTransport::~VerifiedTlsTransport() {
	stop();
#if USE_MBEDTLS
	mbedtls_x509_crt_free(&mCaCert);
#endif
}

} // namespace rtc::impl

#endif
