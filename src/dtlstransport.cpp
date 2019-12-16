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
#include "icetransport.hpp"

#include <cassert>
#include <cstring>
#include <exception>
#include <iostream>

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::weak_ptr;

#if USE_GNUTLS

#include <gnutls/dtls.h>

namespace {

static bool check_gnutls(int ret, const string &message = "GnuTLS error") {
	if (ret < 0) {
		if (!gnutls_error_is_fatal(ret))
			return false;
		throw std::runtime_error(message + ": " + gnutls_strerror(ret));
	}
	return true;
}

} // namespace

namespace rtc {

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, shared_ptr<Certificate> certificate,
                             verifier_callback verifierCallback,
                             state_callback stateChangeCallback)
    : Transport(lower), mCertificate(certificate), mState(State::Disconnected),
      mVerifierCallback(std::move(verifierCallback)),
      mStateChangeCallback(std::move(stateChangeCallback)) {
	gnutls_certificate_set_verify_function(mCertificate->credentials(), CertificateCallback);

	bool active = lower->role() == Description::Role::Active;
	unsigned int flags = GNUTLS_DATAGRAM | (active ? GNUTLS_CLIENT : GNUTLS_SERVER);
	check_gnutls(gnutls_init(&mSession, flags));

	// RFC 8261: SCTP performs segmentation and reassembly based on the path MTU.
	// Therefore, the DTLS layer MUST NOT use any compression algorithm.
	// See https://tools.ietf.org/html/rfc8261#section-5
	const char *priorities = "SECURE128:-VERS-SSL3.0:-ARCFOUR-128:-COMP-ALL";
	const char *err_pos = NULL;
	check_gnutls(gnutls_priority_set_direct(mSession, priorities, &err_pos),
	             "Unable to set TLS priorities");

	check_gnutls(
	    gnutls_credentials_set(mSession, GNUTLS_CRD_CERTIFICATE, mCertificate->credentials()));

	gnutls_dtls_set_mtu(mSession, 1280 - 40 - 8); // min MTU over UDP/IPv6 (only for handshake)
	gnutls_dtls_set_timeouts(mSession, 400, 60000);
	gnutls_handshake_set_timeout(mSession, 60000);

	gnutls_session_set_ptr(mSession, this);
	gnutls_transport_set_ptr(mSession, this);
	gnutls_transport_set_push_function(mSession, WriteCallback);
	gnutls_transport_set_pull_function(mSession, ReadCallback);
	gnutls_transport_set_pull_timeout_function(mSession, TimeoutCallback);

	mRecvThread = std::thread(&DtlsTransport::runRecvLoop, this);
}

DtlsTransport::~DtlsTransport() {
	stop();

	gnutls_bye(mSession, GNUTLS_SHUT_RDWR);
	gnutls_deinit(mSession);
}

DtlsTransport::State DtlsTransport::state() const { return mState; }

void DtlsTransport::stop() {
	Transport::stop();

	if (mRecvThread.joinable()) {
		mIncomingQueue.stop();
		mRecvThread.join();
	}
}

bool DtlsTransport::send(message_ptr message) {
	if (!message || mState != State::Connected)
		return false;

	ssize_t ret;
	do {
		ret = gnutls_record_send(mSession, message->data(), message->size());
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	if (ret == GNUTLS_E_LARGE_PACKET)
		return false;

	return check_gnutls(ret);
}

void DtlsTransport::incoming(message_ptr message) { mIncomingQueue.push(message); }

void DtlsTransport::changeState(State state) {
	if (mState.exchange(state) != state)
		mStateChangeCallback(state);
}

void DtlsTransport::runRecvLoop() {
	const size_t maxMtu = 4096;

	// Handshake loop
	try {
		changeState(State::Connecting);

		int ret;
		do {
			ret = gnutls_handshake(mSession);

			if (ret == GNUTLS_E_LARGE_PACKET)
				throw std::runtime_error("MTU is too low");

		} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN ||
		         !check_gnutls(ret, "TLS handshake failed"));

		// RFC 8261: DTLS MUST support sending messages larger than the current path MTU
		// See https://tools.ietf.org/html/rfc8261#section-5
		gnutls_dtls_set_mtu(mSession, maxMtu + 1);

	} catch (const std::exception &e) {
		std::cerr << "DTLS handshake: " << e.what() << std::endl;
		changeState(State::Failed);
		return;
	}

	// Receive loop
	try {
		changeState(State::Connected);

		const size_t bufferSize = maxMtu;
		char buffer[bufferSize];

		while (true) {
			ssize_t ret;
			do {
				ret = gnutls_record_recv(mSession, buffer, bufferSize);
			} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

			// Consider premature termination as remote closing
			if (ret == GNUTLS_E_PREMATURE_TERMINATION)
				break;

			if (check_gnutls(ret)) {
				if (ret == 0) {
					// Closed
					break;
				}
				auto *b = reinterpret_cast<byte *>(buffer);
				recv(make_message(b, b + ret));
			}
		}

	} catch (const std::exception &e) {
		std::cerr << "DTLS recv: " << e.what() << std::endl;
	}

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
	check_gnutls(gnutls_x509_crt_init(&crt));
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
	auto next = t->mIncomingQueue.pop();
	auto message = next ? *next : nullptr;
	if (!message) {
		// Closed
		gnutls_transport_set_errno(t->mSession, 0);
		return 0;
	}

	ssize_t len = std::min(maxlen, message->size());
	std::memcpy(data, message->data(), len);
	gnutls_transport_set_errno(t->mSession, 0);
	return len;
}

int DtlsTransport::TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	if (ms != GNUTLS_INDEFINITE_TIMEOUT)
		t->mIncomingQueue.wait(std::chrono::milliseconds(ms));
	else
		t->mIncomingQueue.wait();
	return !t->mIncomingQueue.empty() ? 1 : 0;
}

} // namespace rtc

#else

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace {

const int BIO_EOF = -1;

string openssl_error_string(unsigned long err) {
	const size_t bufferSize = 256;
	char buffer[bufferSize];
	ERR_error_string_n(err, buffer, bufferSize);
	return string(buffer);
}

bool check_openssl(int success, const string &message = "OpenSSL error") {
	if (success)
		return true;
	else
		throw std::runtime_error(message + ": " + openssl_error_string(ERR_get_error()));
}

bool check_openssl_ret(SSL *ssl, int ret, const string &message = "OpenSSL error") {
	if (ret == BIO_EOF)
		return true;

	unsigned long err = SSL_get_error(ssl, ret);
	if (err == SSL_ERROR_NONE || err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		return true;
	else if (err == SSL_ERROR_ZERO_RETURN)
		return false;
	else
		throw std::runtime_error(message + ": " + openssl_error_string(err));
}

} // namespace

namespace rtc {

int DtlsTransport::TransportExIndex = -1;
std::mutex DtlsTransport::GlobalMutex;

void DtlsTransport::GlobalInit() {
	std::lock_guard lock(GlobalMutex);
	if (TransportExIndex < 0) {
		TransportExIndex = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	}
}

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, shared_ptr<Certificate> certificate,
                             verifier_callback verifierCallback, state_callback stateChangeCallback)
    : Transport(lower), mCertificate(certificate), mState(State::Disconnected),
      mVerifierCallback(std::move(verifierCallback)),
      mStateChangeCallback(std::move(stateChangeCallback)) {

	GlobalInit();

	if (!(mCtx = SSL_CTX_new(DTLS_method())))
		throw std::runtime_error("Unable to create SSL context");

	check_openssl(SSL_CTX_set_cipher_list(mCtx, "ALL:!LOW:!EXP:!RC4:!MD5:@STRENGTH"),
	              "Unable to set SSL priorities");

	// RFC 8261: SCTP performs segmentation and reassembly based on the path MTU.
	// Therefore, the DTLS layer MUST NOT use any compression algorithm.
	// See https://tools.ietf.org/html/rfc8261#section-5
	SSL_CTX_set_options(mCtx, SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
	SSL_CTX_set_min_proto_version(mCtx, DTLS1_VERSION);
	SSL_CTX_set_read_ahead(mCtx, 1);
	SSL_CTX_set_quiet_shutdown(mCtx, 1);
	SSL_CTX_set_info_callback(mCtx, InfoCallback);
	SSL_CTX_set_verify(mCtx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
	                   CertificateCallback);
	SSL_CTX_set_verify_depth(mCtx, 1);

	auto [x509, pkey] = mCertificate->credentials();
	SSL_CTX_use_certificate(mCtx, x509);
	SSL_CTX_use_PrivateKey(mCtx, pkey);

	check_openssl(SSL_CTX_check_private_key(mCtx), "SSL local private key check failed");

	if (!(mSsl = SSL_new(mCtx)))
	    throw std::runtime_error("Unable to create SSL instance");

	SSL_set_ex_data(mSsl, TransportExIndex, this);
	SSL_set_mtu(mSsl, 1280 - 40 - 8); // min MTU over UDP/IPv6

	if (lower->role() == Description::Role::Active)
		SSL_set_connect_state(mSsl);
	else
		SSL_set_accept_state(mSsl);

    if (!(mInBio = BIO_new(BIO_s_mem())) || !(mOutBio = BIO_new(BIO_s_mem())))
	    throw std::runtime_error("Unable to create BIO");

	BIO_set_mem_eof_return(mInBio, BIO_EOF);
	BIO_set_mem_eof_return(mOutBio, BIO_EOF);
	SSL_set_bio(mSsl, mInBio, mOutBio);

	auto ecdh = unique_ptr<EC_KEY, decltype(&EC_KEY_free)>(
	    EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
	SSL_set_options(mSsl, SSL_OP_SINGLE_ECDH_USE);
	SSL_set_tmp_ecdh(mSsl, ecdh.get());

	mRecvThread = std::thread(&DtlsTransport::runRecvLoop, this);
}

DtlsTransport::~DtlsTransport() {
	stop();

	SSL_shutdown(mSsl);
	SSL_free(mSsl);
	SSL_CTX_free(mCtx);
}

void DtlsTransport::stop() {
	Transport::stop();

	if (mRecvThread.joinable()) {
		mIncomingQueue.stop();
		mRecvThread.join();
	}
}

DtlsTransport::State DtlsTransport::state() const { return mState; }

bool DtlsTransport::send(message_ptr message) {
	const size_t bufferSize = 4096;
	byte buffer[bufferSize];

	if (!message || mState != State::Connected)
		return false;

	int ret = SSL_write(mSsl, message->data(), message->size());
	if (!check_openssl_ret(mSsl, ret)) {
		return false;
	}

	while (BIO_ctrl_pending(mOutBio) > 0) {
		int ret = BIO_read(mOutBio, buffer, bufferSize);
		if (check_openssl_ret(mSsl, ret) && ret > 0)
			outgoing(make_message(buffer, buffer + ret));
	}

	return true;
}

void DtlsTransport::incoming(message_ptr message) { mIncomingQueue.push(message); }

void DtlsTransport::changeState(State state) {
	if (mState.exchange(state) != state)
		mStateChangeCallback(state);
}

void DtlsTransport::runRecvLoop() {
	const size_t bufferSize = 4096;
	byte buffer[bufferSize];

	try {
		changeState(State::Connecting);

		SSL_do_handshake(mSsl);
		while (BIO_ctrl_pending(mOutBio) > 0) {
			int ret = BIO_read(mOutBio, buffer, bufferSize);
			if (check_openssl_ret(mSsl, ret) && ret > 0)
				outgoing(make_message(buffer, buffer + ret));
		}

		while (auto next = mIncomingQueue.pop()) {
			auto message = *next;
			BIO_write(mInBio, message->data(), message->size());
			int ret = SSL_read(mSsl, buffer, bufferSize);
			if (!check_openssl_ret(mSsl, ret))
				break;

			auto decrypted = ret > 0 ? make_message(buffer, buffer + ret) : nullptr;

			if (mState == State::Connecting) {
				if (unsigned long err = ERR_get_error())
					throw std::runtime_error("handshake failed: " + openssl_error_string(err));

				while (BIO_ctrl_pending(mOutBio) > 0) {
					ret = BIO_read(mOutBio, buffer, bufferSize);
					if (check_openssl_ret(mSsl, ret) && ret > 0)
						outgoing(make_message(buffer, buffer + ret));
				}

				if (SSL_is_init_finished(mSsl))
					changeState(State::Connected);
			}

			if (decrypted)
				recv(decrypted);
		}
	} catch (const std::exception &e) {
		std::cerr << "DTLS recv: " << e.what() << std::endl;
	}

	if (mState == State::Connected) {
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		changeState(State::Failed);
	}
}

int DtlsTransport::CertificateCallback(int preverify_ok, X509_STORE_CTX *ctx) {
	SSL *ssl =
	    static_cast<SSL *>(X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
	DtlsTransport *t =
	    static_cast<DtlsTransport *>(SSL_get_ex_data(ssl, DtlsTransport::TransportExIndex));

	X509 *crt = X509_STORE_CTX_get_current_cert(ctx);
	std::string fingerprint = make_fingerprint(crt);

	return t->mVerifierCallback(fingerprint) ? 1 : 0;
}

void DtlsTransport::InfoCallback(const SSL *ssl, int where, int ret) {
	DtlsTransport *t =
	    static_cast<DtlsTransport *>(SSL_get_ex_data(ssl, DtlsTransport::TransportExIndex));

	if (where & SSL_CB_ALERT) {
		if (ret != 256) // Close Notify
			std::cerr << "DTLS alert: " << SSL_alert_desc_string_long(ret) << std::endl;
		t->mIncomingQueue.stop(); // Close the connection
	}
}

} // namespace rtc

#endif

