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

#include "dtlstransport.hpp"
#include "internals.hpp"
#include "icetransport.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>

#if !USE_GNUTLS
#ifdef _WIN32
#include <winsock2.h> // for timeval
#else
#include <sys/time.h> // for timeval
#endif
#endif

using namespace std::chrono;

namespace rtc::impl {

#if USE_GNUTLS

void DtlsTransport::Init() {
	gnutls_global_init(); // optional
}

void DtlsTransport::Cleanup() { gnutls_global_deinit(); }

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
                             optional<size_t> mtu, verifier_callback verifierCallback,
                             state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mMtu(mtu), mCertificate(certificate),
      mVerifierCallback(std::move(verifierCallback)),
      mIsClient(lower->role() == Description::Role::Active), mCurrentDscp(0) {

	PLOG_DEBUG << "Initializing DTLS transport (GnuTLS)";

	gnutls_certificate_credentials_t creds = mCertificate->credentials();
	gnutls_certificate_set_verify_function(creds, CertificateCallback);

	unsigned int flags = GNUTLS_DATAGRAM | (mIsClient ? GNUTLS_CLIENT : GNUTLS_SERVER);
	gnutls::check(gnutls_init(&mSession, flags));

	try {
		// RFC 8261: SCTP performs segmentation and reassembly based on the path MTU.
		// Therefore, the DTLS layer MUST NOT use any compression algorithm.
		// See https://tools.ietf.org/html/rfc8261#section-5
		const char *priorities = "SECURE128:-VERS-SSL3.0:-ARCFOUR-128:-COMP-ALL:+COMP-NULL";
		const char *err_pos = NULL;
		gnutls::check(gnutls_priority_set_direct(mSession, priorities, &err_pos),
		              "Failed to set TLS priorities");

		// RFC 8827: The DTLS-SRTP protection profile SRTP_AES128_CM_HMAC_SHA1_80 MUST be supported
		// See https://tools.ietf.org/html/rfc8827#section-6.5
		gnutls::check(gnutls_srtp_set_profile(mSession, GNUTLS_SRTP_AES128_CM_HMAC_SHA1_80),
		              "Failed to set SRTP profile");

		gnutls::check(gnutls_credentials_set(mSession, GNUTLS_CRD_CERTIFICATE, creds));

		gnutls_dtls_set_timeouts(mSession,
		                         1000,   // 1s retransmission timeout recommended by RFC 6347
		                         30000); // 30s total timeout
		gnutls_handshake_set_timeout(mSession, 30000);

		gnutls_session_set_ptr(mSession, this);
		gnutls_transport_set_ptr(mSession, this);
		gnutls_transport_set_push_function(mSession, WriteCallback);
		gnutls_transport_set_pull_function(mSession, ReadCallback);
		gnutls_transport_set_pull_timeout_function(mSession, TimeoutCallback);

	} catch (...) {
		gnutls_deinit(mSession);
		throw;
	}
}

DtlsTransport::~DtlsTransport() {
	stop();

	gnutls_deinit(mSession);
}

void DtlsTransport::start() {
	Transport::start();

	registerIncoming();

	PLOG_DEBUG << "Starting DTLS recv thread";
	mRecvThread = std::thread(&DtlsTransport::runRecvLoop, this);
}

bool DtlsTransport::stop() {
	if (!Transport::stop())
		return false;

	PLOG_DEBUG << "Stopping DTLS recv thread";
	mIncomingQueue.stop();
	mRecvThread.join();
	return true;
}

bool DtlsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();

	ssize_t ret;
	do {
		std::lock_guard lock(mSendMutex);
		mCurrentDscp = message->dscp;
		ret = gnutls_record_send(mSession, message->data(), message->size());
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	if (ret == GNUTLS_E_LARGE_PACKET)
		return false;

	return gnutls::check(ret);
}

void DtlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
}

bool DtlsTransport::outgoing(message_ptr message) {
	if (message->dscp == 0)
		message->dscp = mCurrentDscp;

	return Transport::outgoing(std::move(message));
}

void DtlsTransport::postHandshake() {
	// Dummy
}

void DtlsTransport::runRecvLoop() {
	const size_t bufferSize = 4096;

	// Handshake loop
	try {
		changeState(State::Connecting);

		size_t mtu = mMtu.value_or(DEFAULT_MTU) - 8 - 40; // UDP/IPv6
		gnutls_dtls_set_mtu(mSession, static_cast<unsigned int>(mtu));
		PLOG_VERBOSE << "SSL MTU set to " << mtu;

		int ret;
		do {
			ret = gnutls_handshake(mSession);

			if (ret == GNUTLS_E_LARGE_PACKET)
				throw std::runtime_error("MTU is too low");

		} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN ||
		         !gnutls::check(ret, "DTLS handshake failed"));

		// RFC 8261: DTLS MUST support sending messages larger than the current path MTU
		// See https://tools.ietf.org/html/rfc8261#section-5
		gnutls_dtls_set_mtu(mSession, bufferSize + 1);

	} catch (const std::exception &e) {
		PLOG_ERROR << "DTLS handshake: " << e.what();
		changeState(State::Failed);
		return;
	}

	// Receive loop
	try {
		PLOG_INFO << "DTLS handshake finished";
		postHandshake();
		changeState(State::Connected);

		char buffer[bufferSize];

		while (true) {
			ssize_t ret;
			do {
				ret = gnutls_record_recv(mSession, buffer, bufferSize);
			} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

			// RFC 8827: Implementations MUST NOT implement DTLS renegotiation and MUST reject it
			// with a "no_renegotiation" alert if offered.
			// See https://tools.ietf.org/html/rfc8827#section-6.5
			if (ret == GNUTLS_E_REHANDSHAKE) {
				do {
					std::lock_guard lock(mSendMutex);
					ret = gnutls_alert_send(mSession, GNUTLS_AL_WARNING, GNUTLS_A_NO_RENEGOTIATION);
				} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);
				continue;
			}

			// Consider premature termination as remote closing
			if (ret == GNUTLS_E_PREMATURE_TERMINATION) {
				PLOG_DEBUG << "DTLS connection terminated";
				break;
			}

			if (gnutls::check(ret)) {
				if (ret == 0) {
					// Closed
					PLOG_DEBUG << "DTLS connection cleanly closed";
					break;
				}
				auto *b = reinterpret_cast<byte *>(buffer);
				recv(make_message(b, b + ret));
			}
		}

	} catch (const std::exception &e) {
		PLOG_ERROR << "DTLS recv: " << e.what();
	}

	gnutls_bye(mSession, GNUTLS_SHUT_RDWR);

	PLOG_INFO << "DTLS closed";
	changeState(State::Disconnected);
	recv(nullptr);
}

int DtlsTransport::CertificateCallback(gnutls_session_t session) {
	DtlsTransport *t = static_cast<DtlsTransport *>(gnutls_session_get_ptr(session));

	if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	unsigned int count = 0;
	const gnutls_datum_t *array = gnutls_certificate_get_peers(session, &count);
	if (!array || count == 0) {
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	gnutls_x509_crt_t crt;
	gnutls::check(gnutls_x509_crt_init(&crt));
	int ret = gnutls_x509_crt_import(crt, &array[0], GNUTLS_X509_FMT_DER);
	if (ret != GNUTLS_E_SUCCESS) {
		gnutls_x509_crt_deinit(crt);
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	string fingerprint = make_fingerprint(crt);
	gnutls_x509_crt_deinit(crt);

	bool success = t->mVerifierCallback(fingerprint);
	return success ? GNUTLS_E_SUCCESS : GNUTLS_E_CERTIFICATE_ERROR;
}

ssize_t DtlsTransport::WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	if (len > 0) {
		auto b = reinterpret_cast<const byte *>(data);
		t->outgoing(make_message(b, b + len));
	}
	gnutls_transport_set_errno(t->mSession, 0);
	return ssize_t(len);
}

ssize_t DtlsTransport::ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	if (auto next = t->mIncomingQueue.pop()) {
		message_ptr message = std::move(*next);
		ssize_t len = std::min(maxlen, message->size());
		std::memcpy(data, message->data(), len);
		gnutls_transport_set_errno(t->mSession, 0);
		return len;
	}
	// Closed
	gnutls_transport_set_errno(t->mSession, 0);
	return 0;
}

int DtlsTransport::TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	bool notEmpty = t->mIncomingQueue.wait(
	    ms != GNUTLS_INDEFINITE_TIMEOUT ? std::make_optional(milliseconds(ms)) : nullopt);
	return notEmpty ? 1 : 0;
}

#else // USE_GNUTLS==0

BIO_METHOD *DtlsTransport::BioMethods = NULL;
int DtlsTransport::TransportExIndex = -1;
std::mutex DtlsTransport::GlobalMutex;

void DtlsTransport::Init() {
	std::lock_guard lock(GlobalMutex);

	openssl::init();

	if (!BioMethods) {
		BioMethods = BIO_meth_new(BIO_TYPE_BIO, "DTLS writer");
		if (!BioMethods)
			throw std::runtime_error("Failed to create BIO methods for DTLS writer");
		BIO_meth_set_create(BioMethods, BioMethodNew);
		BIO_meth_set_destroy(BioMethods, BioMethodFree);
		BIO_meth_set_write(BioMethods, BioMethodWrite);
		BIO_meth_set_ctrl(BioMethods, BioMethodCtrl);
	}
	if (TransportExIndex < 0) {
		TransportExIndex = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	}
}

void DtlsTransport::Cleanup() {
	// Nothing to do
}

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, shared_ptr<Certificate> certificate,
                             optional<size_t> mtu, verifier_callback verifierCallback,
                             state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mMtu(mtu), mCertificate(certificate),
      mVerifierCallback(std::move(verifierCallback)),
      mIsClient(lower->role() == Description::Role::Active), mCurrentDscp(0) {
	PLOG_DEBUG << "Initializing DTLS transport (OpenSSL)";

	try {
		mCtx = SSL_CTX_new(DTLS_method());
		if (!mCtx)
			throw std::runtime_error("Failed to create SSL context");

		// RFC 8261: SCTP performs segmentation and reassembly based on the path MTU.
		// Therefore, the DTLS layer MUST NOT use any compression algorithm.
		// See https://tools.ietf.org/html/rfc8261#section-5
		// RFC 8827: Implementations MUST NOT implement DTLS renegotiation
		// See https://tools.ietf.org/html/rfc8827#section-6.5
		SSL_CTX_set_options(mCtx, SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_NO_QUERY_MTU |
		                              SSL_OP_NO_RENEGOTIATION);

		SSL_CTX_set_min_proto_version(mCtx, DTLS1_VERSION);
		SSL_CTX_set_read_ahead(mCtx, 1);
		SSL_CTX_set_quiet_shutdown(mCtx, 1);
		SSL_CTX_set_info_callback(mCtx, InfoCallback);

		SSL_CTX_set_verify(mCtx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
		                   CertificateCallback);
		SSL_CTX_set_verify_depth(mCtx, 1);

		openssl::check(SSL_CTX_set_cipher_list(mCtx, "ALL:!LOW:!EXP:!RC4:!MD5:@STRENGTH"),
		               "Failed to set SSL priorities");

		auto [x509, pkey] = mCertificate->credentials();
		SSL_CTX_use_certificate(mCtx, x509);
		SSL_CTX_use_PrivateKey(mCtx, pkey);

		openssl::check(SSL_CTX_check_private_key(mCtx), "SSL local private key check failed");

		mSsl = SSL_new(mCtx);
		if (!mSsl)
			throw std::runtime_error("Failed to create SSL instance");

		SSL_set_ex_data(mSsl, TransportExIndex, this);

		if (mIsClient)
			SSL_set_connect_state(mSsl);
		else
			SSL_set_accept_state(mSsl);

		mInBio = BIO_new(BIO_s_mem());
		mOutBio = BIO_new(BioMethods);
		if (!mInBio || !mOutBio)
			throw std::runtime_error("Failed to create BIO");

		BIO_set_mem_eof_return(mInBio, BIO_EOF);
		BIO_set_data(mOutBio, this);
		SSL_set_bio(mSsl, mInBio, mOutBio);

		auto ecdh = unique_ptr<EC_KEY, decltype(&EC_KEY_free)>(
		    EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
		SSL_set_options(mSsl, SSL_OP_SINGLE_ECDH_USE);
		SSL_set_tmp_ecdh(mSsl, ecdh.get());

		// RFC 8827: The DTLS-SRTP protection profile SRTP_AES128_CM_HMAC_SHA1_80 MUST be supported
		// See https://tools.ietf.org/html/rfc8827#section-6.5 Warning:
		// SSL_set_tlsext_use_srtp() returns 0 on success and 1 on error
		if (SSL_set_tlsext_use_srtp(mSsl, "SRTP_AES128_CM_SHA1_80"))
			throw std::runtime_error("Failed to set SRTP profile: " +
			                         openssl::error_string(ERR_get_error()));

	} catch (...) {
		if (mSsl)
			SSL_free(mSsl);
		if (mCtx)
			SSL_CTX_free(mCtx);
		throw;
	}
}

DtlsTransport::~DtlsTransport() {
	stop();

	SSL_free(mSsl);
	SSL_CTX_free(mCtx);
}

void DtlsTransport::start() {
	Transport::start();

	registerIncoming();

	PLOG_DEBUG << "Starting DTLS recv thread";
	mRecvThread = std::thread(&DtlsTransport::runRecvLoop, this);
}

bool DtlsTransport::stop() {
	if (!Transport::stop())
		return false;

	PLOG_DEBUG << "Stopping DTLS recv thread";
	mIncomingQueue.stop();
	mRecvThread.join();
	SSL_shutdown(mSsl);
	return true;
}

bool DtlsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();

	mCurrentDscp = message->dscp;
	int ret = SSL_write(mSsl, message->data(), int(message->size()));
	return openssl::check(mSsl, ret);
}

void DtlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
}

bool DtlsTransport::outgoing(message_ptr message) {
	if (message->dscp == 0)
		message->dscp = mCurrentDscp;

	return Transport::outgoing(std::move(message));
}

void DtlsTransport::postHandshake() {
	// Dummy
}

void DtlsTransport::runRecvLoop() {
	const size_t bufferSize = 4096;
	try {
		changeState(State::Connecting);

		size_t mtu = mMtu.value_or(DEFAULT_MTU) - 8 - 40; // UDP/IPv6
		SSL_set_mtu(mSsl, static_cast<unsigned int>(mtu));
		PLOG_VERBOSE << "SSL MTU set to " << mtu;

		// Initiate the handshake
		int ret = SSL_do_handshake(mSsl);
		openssl::check(mSsl, ret, "Handshake failed");

		byte buffer[bufferSize];
		while (mIncomingQueue.running()) {
			// Process pending messages
			while (auto next = mIncomingQueue.tryPop()) {
				message_ptr message = std::move(*next);
				BIO_write(mInBio, message->data(), int(message->size()));

				if (state() == State::Connecting) {
					// Continue the handshake
					ret = SSL_do_handshake(mSsl);
					if (!openssl::check(mSsl, ret, "Handshake failed"))
						break;

					if (SSL_is_init_finished(mSsl)) {
						// RFC 8261: DTLS MUST support sending messages larger than the current path
						// MTU See https://tools.ietf.org/html/rfc8261#section-5
						SSL_set_mtu(mSsl, bufferSize + 1);

						PLOG_INFO << "DTLS handshake finished";
						postHandshake();
						changeState(State::Connected);
					}
				} else {
					ret = SSL_read(mSsl, buffer, bufferSize);
					if (!openssl::check(mSsl, ret))
						break;

					if (ret > 0)
						recv(make_message(buffer, buffer + ret));
				}
			}

			// No more messages pending, retransmit and rearm timeout if connecting
			optional<milliseconds> duration;
			if (state() == State::Connecting) {
				// Warning: This function breaks the usual return value convention
				ret = DTLSv1_handle_timeout(mSsl);
				if (ret < 0) {
					throw std::runtime_error("Handshake timeout"); // write BIO can't fail
				} else if (ret > 0) {
					LOG_VERBOSE << "OpenSSL did DTLS retransmit";
				}

				struct timeval timeout = {};
				if (state() == State::Connecting && DTLSv1_get_timeout(mSsl, &timeout)) {
					duration = milliseconds(timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
					// Also handle handshake timeout manually because OpenSSL actually doesn't...
					// OpenSSL backs off exponentially in base 2 starting from the recommended 1s
					// so this allows for 5 retransmissions and fails after roughly 30s.
					if (duration > 30s) {
						throw std::runtime_error("Handshake timeout");
					} else {
						LOG_VERBOSE << "OpenSSL DTLS retransmit timeout is " << duration->count()
						            << "ms";
					}
				}
			}

			mIncomingQueue.wait(duration);
		}
	} catch (const std::exception &e) {
		PLOG_ERROR << "DTLS recv: " << e.what();
	}

	if (state() == State::Connected) {
		PLOG_INFO << "DTLS closed";
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		PLOG_ERROR << "DTLS handshake failed";
		changeState(State::Failed);
	}
}

int DtlsTransport::CertificateCallback(int /*preverify_ok*/, X509_STORE_CTX *ctx) {
	SSL *ssl =
	    static_cast<SSL *>(X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
	DtlsTransport *t =
	    static_cast<DtlsTransport *>(SSL_get_ex_data(ssl, DtlsTransport::TransportExIndex));

	X509 *crt = X509_STORE_CTX_get_current_cert(ctx);
	string fingerprint = make_fingerprint(crt);

	return t->mVerifierCallback(fingerprint) ? 1 : 0;
}

void DtlsTransport::InfoCallback(const SSL *ssl, int where, int ret) {
	DtlsTransport *t =
	    static_cast<DtlsTransport *>(SSL_get_ex_data(ssl, DtlsTransport::TransportExIndex));

	if (where & SSL_CB_ALERT) {
		if (ret != 256) { // Close Notify
			PLOG_ERROR << "DTLS alert: " << SSL_alert_desc_string_long(ret);
		}
		t->mIncomingQueue.stop(); // Close the connection
	}
}

int DtlsTransport::BioMethodNew(BIO *bio) {
	BIO_set_init(bio, 1);
	BIO_set_data(bio, NULL);
	BIO_set_shutdown(bio, 0);
	return 1;
}

int DtlsTransport::BioMethodFree(BIO *bio) {
	if (!bio)
		return 0;
	BIO_set_data(bio, NULL);
	return 1;
}

int DtlsTransport::BioMethodWrite(BIO *bio, const char *in, int inl) {
	if (inl <= 0)
		return inl;
	auto transport = reinterpret_cast<DtlsTransport *>(BIO_get_data(bio));
	if (!transport)
		return -1;
	auto b = reinterpret_cast<const byte *>(in);
	transport->outgoing(make_message(b, b + inl));
	return inl; // can't fail
}

long DtlsTransport::BioMethodCtrl(BIO * /*bio*/, int cmd, long /*num*/, void * /*ptr*/) {
	switch (cmd) {
	case BIO_CTRL_FLUSH:
		return 1;
	case BIO_CTRL_DGRAM_QUERY_MTU:
		return 0; // SSL_OP_NO_QUERY_MTU must be set
	case BIO_CTRL_WPENDING:
	case BIO_CTRL_PENDING:
		return 0;
	default:
		break;
	}
	return 0;
}

#endif

} // namespace rtc::impl
