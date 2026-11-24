/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "verifiedtlstransport.hpp"
#include "common.hpp"
#include "internals.hpp"
#include "utils.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

static const string PemBeginCertificateTag = "-----BEGIN CERTIFICATE-----";

#if USE_MBEDTLS

namespace {

string verify_info(uint32_t flags) {
	const size_t bufferSize = 1024;
	char buffer[bufferSize];
	if (mbedtls_x509_crt_verify_info(buffer, bufferSize, "", flags) < 0)
		return "unknown error";

	// One line per flag
	string info;
	for (const auto &line : utils::explode(buffer, '\n')) {
		if (line.empty())
			continue;
		if (!info.empty())
			info += ", ";
		info += line;
	}
	return info;
}

#if RTC_SYSTEM_TRUST_STORE

uint32_t to_mbedtls_flags(system_trust::Status status) {
	using system_trust::Status;
	switch (status) {
	case Status::Trusted:
		return 0;
	case Status::UntrustedRoot:
		return MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	case Status::HostnameMismatch:
		return MBEDTLS_X509_BADCERT_CN_MISMATCH;
	case Status::Expired:
		return MBEDTLS_X509_BADCERT_EXPIRED;
	case Status::NotYetValid:
		return MBEDTLS_X509_BADCERT_FUTURE;
	case Status::Revoked:
		return MBEDTLS_X509_BADCERT_REVOKED;
	default:
		return MBEDTLS_X509_BADCERT_OTHER;
	}
}

#else

// Load the system CA certificates once, they are then shared read-only between transports
shared_ptr<mbedtls_x509_crt> system_ca_certificates() {
	static std::mutex mutex;
	static bool loaded = false;
	static shared_ptr<mbedtls_x509_crt> certs;

	std::lock_guard lock(mutex);
	if (std::exchange(loaded, true))
		return certs;

	// A positive return value is the number of certificates which failed to parse
	for (const auto &file : system_trust::ca_bundle_files()) {
		auto crt = mbedtls::new_x509_crt();
		if (mbedtls_x509_crt_parse_file(crt.get(), file.c_str()) >= 0 && crt->raw.len > 0) {
			PLOG_DEBUG << "Loaded system CA certificates from " << file;
			certs = std::move(crt);
			return certs;
		}
	}

	for (const auto &directory : system_trust::ca_bundle_directories()) {
		auto crt = mbedtls::new_x509_crt();
		if (mbedtls_x509_crt_parse_path(crt.get(), directory.c_str()) >= 0 && crt->raw.len > 0) {
			PLOG_DEBUG << "Loaded system CA certificates from " << directory;
			certs = std::move(crt);
			return certs;
		}
	}

	PLOG_WARNING << "System CA certificates unavailable";
	return certs;
}

#endif

} // namespace

#elif !USE_GNUTLS && RTC_SYSTEM_TRUST_STORE

namespace {

binary to_der(X509 *cert) {
	int len = i2d_X509(cert, nullptr);
	if (len <= 0)
		return {};

	binary der(size_t(len), byte(0));
	auto *p = reinterpret_cast<unsigned char *>(der.data());
	if (i2d_X509(cert, &p) != len)
		return {};

	return der;
}

} // namespace

#endif

VerifiedTlsTransport::VerifiedTlsTransport(
    variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>> lower, string host,
    certificate_ptr certificate, state_callback callback, [[maybe_unused]] optional<string> cacert)
    : TlsTransport(std::move(lower), std::move(host), std::move(certificate), std::move(callback)) {

	PLOG_DEBUG << "Setting up TLS certificate verification";

#if USE_GNUTLS
	gnutls_session_set_verify_cert(mSession, mHost->c_str(), 0);

#elif USE_MBEDTLS
	mbedtls_ssl_conf_authmode(&mConf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_verify(&mConf, VerifyCallback, this);
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
			mHasCaCert = true;
		}

		// Mbed TLS requires a CA chain to verify the peer, it is empty if there is no custom CA
		// certificate. With a system trust store, chains rejected by Mbed TLS are then checked
		// against the system trust store in the verification callback. Otherwise, the system CA
		// certificates are used directly, unless there is a custom CA certificate, in which case
		// they are checked in the verification callback.
		mbedtls_x509_crt *caChain = &mCaCert;
#if !RTC_SYSTEM_TRUST_STORE
		mSystemCaCert = system_ca_certificates();
		if (!mHasCaCert && mSystemCaCert)
			caChain = mSystemCaCert.get();
#endif
		mbedtls_ssl_conf_ca_chain(&mConf, caChain, NULL);

	} catch (...) {
		mbedtls_x509_crt_free(&mCaCert);
		throw;
	}

#else // OPENSSL
	if (cacert) {
		if (cacert->find(PemBeginCertificateTag) == string::npos) {
			// *cacert is a file path
			openssl::check(SSL_CTX_load_verify_locations(mCtx, cacert->c_str(), NULL),
			               "Failed to load CA certificate");
		} else {
			// *cacert is a PEM content
			try {
				openssl::SSL_CTX_add_cert_to_store_from_pem(mCtx, *cacert);
			} catch (std::exception const &e) {
				throw std::runtime_error("Failed to add CA certificate to store: " +
				                         std::string(e.what()));
			}
		}
	}

#if RTC_SYSTEM_TRUST_STORE
	// Chains rejected by OpenSSL are checked against the system trust store
	SSL_CTX_set_cert_verify_callback(mCtx, CertVerifyCallback, this);
#endif

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

#if USE_MBEDTLS

int VerifiedTlsTransport::VerifyCallback(void *ctx, mbedtls_x509_crt *crt, int depth,
                                         uint32_t *flags) {
	auto *t = static_cast<VerifiedTlsTransport *>(ctx);

	// Mbed TLS calls the callback for each certificate of the chain it built, from the top of the
	// chain down to the server certificate at depth 0, and the final result is the union of the
	// flags. Flags of upper certificates are moved aside so the whole chain can be accepted or
	// rejected at once at depth 0, which is always called last.
	if (depth > 0) {
		t->mChainFlags |= *flags;
		*flags = 0;
		return 0;
	}

	const uint32_t chainFlags = std::exchange(t->mChainFlags, 0) | *flags;
	*flags = chainFlags;
	if (chainFlags == 0)
		return 0; // Trusted by Mbed TLS

	const string &host = *t->mHost;
	string reason;
#if RTC_SYSTEM_TRUST_STORE
	std::vector<binary> chain;
	for (mbedtls_x509_crt *c = crt; c && c->raw.p; c = c->next) {
		auto *p = reinterpret_cast<const byte *>(c->raw.p);
		chain.emplace_back(p, p + c->raw.len);
	}

	auto result = system_trust::verify_server_chain(chain, host);
	if (result.trusted()) {
		PLOG_DEBUG << "TLS certificate chain for \"" << host << "\" is trusted by the system";
		*flags = 0;
		return 0;
	}

	*flags = chainFlags | to_mbedtls_flags(result.status);
	reason = result.message;
	if (t->mHasCaCert)
		reason += "; CA certificate: " + verify_info(chainFlags);
#else
	if (t->mHasCaCert && t->mSystemCaCert) {
		uint32_t systemFlags = 0;
		if (mbedtls_x509_crt_verify(crt, t->mSystemCaCert.get(), NULL, host.c_str(), &systemFlags,
		                            NULL, NULL) == 0 &&
		    systemFlags == 0) {
			PLOG_DEBUG << "TLS certificate chain for \"" << host << "\" is trusted by the system";
			*flags = 0;
			return 0;
		}
		reason = verify_info(chainFlags | systemFlags);
	} else {
		reason = verify_info(chainFlags);
	}
#endif

	PLOG_WARNING << "TLS certificate verification failed for \"" << host << "\": " << reason;
	return 0;
}

#elif !USE_GNUTLS && RTC_SYSTEM_TRUST_STORE

int VerifiedTlsTransport::CertVerifyCallback(X509_STORE_CTX *ctx, void *arg) {
	auto *t = static_cast<VerifiedTlsTransport *>(arg);

	// Verify with OpenSSL first, against default paths and the custom CA certificate if any. The
	// verification parameters inherited from the SSL object include the host name check.
	if (X509_verify_cert(ctx) > 0)
		return 1;

	const string &host = *t->mHost;
	const int error = X509_STORE_CTX_get_error(ctx);
	X509 *leaf = X509_STORE_CTX_get0_cert(ctx);
	if (!leaf) {
		PLOG_WARNING << "TLS certificate verification failed for \"" << host
		             << "\": " << X509_verify_cert_error_string(error);
		return 0;
	}

	// Leaf certificate first, followed by the other certificates sent by the server
	std::vector<binary> chain;
	chain.emplace_back(to_der(leaf));
	if (STACK_OF(X509) *untrusted = X509_STORE_CTX_get0_untrusted(ctx)) {
		for (int i = 0; i < sk_X509_num(untrusted); ++i) {
			X509 *cert = sk_X509_value(untrusted, i);
			if (X509_cmp(cert, leaf) != 0)
				chain.emplace_back(to_der(cert));
		}
	}

	auto result = system_trust::verify_server_chain(chain, host);
	if (result.trusted()) {
		PLOG_DEBUG << "TLS certificate chain for \"" << host << "\" is trusted by the system";
		ERR_clear_error();
		X509_STORE_CTX_set_error(ctx, X509_V_OK);
		return 1;
	}

	PLOG_WARNING << "TLS certificate verification failed for \"" << host << "\": " << result.message
	             << "; OpenSSL: " << X509_verify_cert_error_string(error);
	return 0;
}

#endif

} // namespace rtc::impl

#endif
