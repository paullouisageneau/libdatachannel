/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dtlstransport.hpp"
#include "dtlssrtptransport.hpp"
#include "icetransport.hpp"
#include "internals.hpp"
#include "threadpool.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>

#if !USE_GNUTLS
#ifdef _WIN32
#include <winsock2.h> // for timeval
#else
#include <sys/time.h> // for timeval
#endif
#endif

using namespace std::chrono;

namespace rtc::impl {

void DtlsTransport::enqueueRecv() {
	if (mPendingRecvCount > 0)
		return;

	++mPendingRecvCount;

	ThreadPool::Instance().enqueue([weak_this = weak_from_this()]() {
		if (auto locked = weak_this.lock())
			locked->doRecv();
	});
}

#if USE_GNUTLS

void DtlsTransport::Init() {
	gnutls_global_init(); // optional
}

void DtlsTransport::Cleanup() { gnutls_global_deinit(); }

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
                             optional<size_t> mtu,
                             CertificateFingerprint::Algorithm fingerprintAlgorithm,
                             verifier_callback verifierCallback, state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mMtu(mtu), mCertificate(std::move(certificate)),
      mFingerprintAlgorithm(fingerprintAlgorithm), mVerifierCallback(std::move(verifierCallback)),
      mIsClient(lower->role() == Description::Role::Active),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing DTLS transport (GnuTLS)";

	if (!mCertificate)
		throw std::invalid_argument("DTLS certificate is null");

	gnutls_certificate_credentials_t creds = mCertificate->credentials();
	gnutls_certificate_set_verify_function(creds, CertificateCallback);

	unsigned int flags =
	    GNUTLS_DATAGRAM | GNUTLS_NONBLOCK | (mIsClient ? GNUTLS_CLIENT : GNUTLS_SERVER);
	gnutls::check(gnutls_init(&mSession, flags));

	try {
		// RFC 8261: SCTP performs segmentation and reassembly based on the path MTU.
		// Therefore, the DTLS layer MUST NOT use any compression algorithm.
		// See https://www.rfc-editor.org/rfc/rfc8261.html#section-5
		const char *priorities = "SECURE128:-VERS-SSL3.0:-ARCFOUR-128:-COMP-ALL:+COMP-NULL";
		const char *err_pos = NULL;
		gnutls::check(gnutls_priority_set_direct(mSession, priorities, &err_pos),
		              "Failed to set TLS priorities");

		// RFC 8827: The DTLS-SRTP protection profile SRTP_AES128_CM_HMAC_SHA1_80 MUST be supported
		// See https://www.rfc-editor.org/rfc/rfc8827.html#section-6.5
		gnutls::check(gnutls_srtp_set_profile(mSession, GNUTLS_SRTP_AES128_CM_HMAC_SHA1_80),
		              "Failed to set SRTP profile");

		gnutls::check(gnutls_credentials_set(mSession, GNUTLS_CRD_CERTIFICATE, creds));
		if (!mIsClient)
			gnutls_certificate_server_set_request(mSession, GNUTLS_CERT_REQUIRE);

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

	// Set recommended medium-priority DSCP value for handshake
	// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
	mCurrentDscp = 10; // AF11: Assured Forwarding class 1, low drop probability
}

DtlsTransport::~DtlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying DTLS transport";
	gnutls_deinit(mSession);
}

void DtlsTransport::start() {
	PLOG_DEBUG << "Starting DTLS transport";

	// Set the MTU
	size_t mtu = mMtu.value_or(DEFAULT_MTU) - 8 - 40; // UDP/IPv6
	gnutls_dtls_set_mtu(mSession, static_cast<unsigned int>(mtu));
	PLOG_VERBOSE << "DTLS MTU set to " << mtu;

	registerIncoming();
	changeState(State::Connecting);

	enqueueRecv(); // to initiate the handshake
}

void DtlsTransport::stop() {
	PLOG_DEBUG << "Stopping DTLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
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

	if (!gnutls::check(ret))
		return false;

	return mOutgoingResult;
}

void DtlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	if (mIncomingQueue.tryPush(std::move(message))) {
		enqueueRecv();
	} else {
		PLOG_VERBOSE << "DTLS incoming queue is full, dropping";
	}
}

bool DtlsTransport::outgoing(message_ptr message) {
	message->dscp = mCurrentDscp;

	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

bool DtlsTransport::demuxMessage(message_ptr) {
	// Dummy
	return false;
}

void DtlsTransport::postHandshake() {
	// Dummy
}

void DtlsTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;

	if (state() != State::Connecting && state() != State::Connected)
		return;

	try {
		const size_t bufferSize = 4096;
		char buffer[bufferSize];

		// Handle handshake if connecting
		if (state() == State::Connecting) {
			int ret;
			do {
				ret = gnutls_handshake(mSession);

				if (ret == GNUTLS_E_AGAIN) {
					// Schedule next call on timeout and return
					auto timeout = milliseconds(gnutls_dtls_get_timeout(mSession));
					ThreadPool::Instance().schedule(timeout, [weak_this = weak_from_this()]() {
						if (auto locked = weak_this.lock())
							locked->doRecv();
					});
					return;
				}

				if (ret == GNUTLS_E_LARGE_PACKET) {
					throw std::runtime_error("MTU is too low");
				}

			} while (!gnutls::check(ret, "Handshake failed")); // Re-call on non-fatal error

			// RFC 8261: DTLS MUST support sending messages larger than the current path MTU
			// See https://www.rfc-editor.org/rfc/rfc8261.html#section-5
			gnutls_dtls_set_mtu(mSession, bufferSize + 1);

			PLOG_INFO << "DTLS handshake finished";
			changeState(State::Connected);
			postHandshake();
		}

		if (state() == State::Connected) {
			while (true) {
				ssize_t ret = gnutls_record_recv(mSession, buffer, bufferSize);

				if (ret == GNUTLS_E_AGAIN) {
					return;
				}

				// RFC 8827: Implementations MUST NOT implement DTLS renegotiation and MUST reject
				// it with a "no_renegotiation" alert if offered. See
				// https://www.rfc-editor.org/rfc/rfc8827.html#section-6.5
				if (ret == GNUTLS_E_REHANDSHAKE) {
					do {
						std::lock_guard lock(mSendMutex);
						ret = gnutls_alert_send(mSession, GNUTLS_AL_WARNING,
						                        GNUTLS_A_NO_RENEGOTIATION);
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
		}
	} catch (const std::exception &e) {
		PLOG_ERROR << "DTLS recv: " << e.what();
	}

	gnutls_bye(mSession, GNUTLS_SHUT_WR);

	if (state() == State::Connected) {
		PLOG_INFO << "DTLS closed";
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		PLOG_ERROR << "DTLS handshake failed";
		changeState(State::Failed);
	}
}

int DtlsTransport::CertificateCallback(gnutls_session_t session) {
	DtlsTransport *t = static_cast<DtlsTransport *>(gnutls_session_get_ptr(session));
	try {
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

		string fingerprint = make_fingerprint(crt, t->mFingerprintAlgorithm);
		gnutls_x509_crt_deinit(crt);

		bool success = t->mVerifierCallback(fingerprint);
		return success ? GNUTLS_E_SUCCESS : GNUTLS_E_CERTIFICATE_ERROR;

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return GNUTLS_E_CERTIFICATE_ERROR;
	}
}

ssize_t DtlsTransport::WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	try {
		if (len > 0) {
			auto b = reinterpret_cast<const byte *>(data);
			t->outgoing(make_message(b, b + len));
		}
		gnutls_transport_set_errno(t->mSession, 0);
		return ssize_t(len);

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		gnutls_transport_set_errno(t->mSession, ECONNRESET);
		return -1;
	}
}

ssize_t DtlsTransport::ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	try {
		while (t->mIncomingQueue.running()) {
			auto next = t->mIncomingQueue.pop();
			if (!next) {
				gnutls_transport_set_errno(t->mSession, EAGAIN);
				return -1;
			}

			message_ptr message = std::move(*next);
			if (t->demuxMessage(message))
				continue;

			ssize_t len = std::min(maxlen, message->size());
			std::memcpy(data, message->data(), len);
			gnutls_transport_set_errno(t->mSession, 0);
			return len;
		}

		// Closed
		gnutls_transport_set_errno(t->mSession, 0);
		return 0;

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		gnutls_transport_set_errno(t->mSession, ECONNRESET);
		return -1;
	}
}

int DtlsTransport::TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int /* ms */) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	try {
		return !t->mIncomingQueue.empty() ? 1 : 0;

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return 1;
	}
}

#elif USE_MBEDTLS

const mbedtls_ssl_srtp_profile srtpSupportedProtectionProfiles[] = {
    MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
    MBEDTLS_TLS_SRTP_UNSET,
};

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
                             optional<size_t> mtu,
                             CertificateFingerprint::Algorithm fingerprintAlgorithm,
                             verifier_callback verifierCallback, state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mMtu(mtu), mCertificate(std::move(certificate)),
      mFingerprintAlgorithm(fingerprintAlgorithm), mVerifierCallback(std::move(verifierCallback)),
      mIsClient(lower->role() == Description::Role::Active),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing DTLS transport (MbedTLS)";

	if (!mCertificate)
		throw std::invalid_argument("DTLS certificate is null");

	mbedtls_ssl_init(&mSsl);
	mbedtls_ssl_config_init(&mConf);

	try {
		mbedtls::check(mbedtls_ssl_config_defaults(
		                   &mConf, mIsClient ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
		                   MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT));

		mbedtls_ssl_conf_max_tls_version(&mConf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_authmode(&mConf, MBEDTLS_SSL_VERIFY_OPTIONAL);
		mbedtls_ssl_conf_verify(&mConf, DtlsTransport::CertificateCallback, this);
#if MBEDTLS_VERSION_MAJOR < 4
		mbedtls_ssl_conf_rng(&mConf, mbedtls::random_func, nullptr);
#endif

		auto [crt, pk] = mCertificate->credentials();
		mbedtls::check(mbedtls_ssl_conf_own_cert(&mConf, crt.get(), pk.get()));

		mbedtls_ssl_conf_dtls_cookies(&mConf, NULL, NULL, NULL);
		mbedtls_ssl_conf_dtls_srtp_protection_profiles(&mConf, srtpSupportedProtectionProfiles);

		mbedtls::check(mbedtls_ssl_setup(&mSsl, &mConf));

		mbedtls_ssl_set_export_keys_cb(&mSsl, DtlsTransport::ExportKeysCallback, this);
		mbedtls_ssl_set_bio(&mSsl, this, WriteCallback, ReadCallback, NULL);
		mbedtls_ssl_set_timer_cb(&mSsl, this, SetTimerCallback, GetTimerCallback);

	} catch (...) {
		mbedtls_ssl_free(&mSsl);
		mbedtls_ssl_config_free(&mConf);
		throw;
	}

	// Set recommended medium-priority DSCP value for handshake
	// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
	mCurrentDscp = 10; // AF11: Assured Forwarding class 1, low drop probability
}

DtlsTransport::~DtlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying DTLS transport";
	mbedtls_ssl_free(&mSsl);
	mbedtls_ssl_config_free(&mConf);
}

void DtlsTransport::Init() {
	// Nothing to do
}

void DtlsTransport::Cleanup() {
	// Nothing to do
}

void DtlsTransport::start() {
	PLOG_DEBUG << "Starting DTLS transport";

	// Set the MTU
	{
		std::lock_guard lock(mSslMutex);
		size_t mtu = mMtu.value_or(DEFAULT_MTU) - 8 - 40; // UDP/IPv6
		mbedtls_ssl_set_mtu(&mSsl, static_cast<unsigned int>(mtu));
		PLOG_VERBOSE << "DTLS MTU set to " << mtu;
	}

	registerIncoming();
	changeState(State::Connecting);

	enqueueRecv(); // to initiate the handshake
}

void DtlsTransport::stop() {
	PLOG_DEBUG << "Stopping DTLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
}

bool DtlsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();

	int ret;
	do {
		std::lock_guard lock(mSslMutex);
		if (message->size() > size_t(mbedtls_ssl_get_max_out_record_payload(&mSsl)))
			return false;

		mCurrentDscp = message->dscp;
		ret = mbedtls_ssl_write(&mSsl, reinterpret_cast<const unsigned char *>(message->data()),
		                        message->size());
	} while (!mbedtls::check(ret));

	return mOutgoingResult;
}

void DtlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	if (mIncomingQueue.tryPush(std::move(message))) {
		enqueueRecv();
	} else {
		PLOG_VERBOSE << "DTLS incoming queue is full, dropping";
	}
}

bool DtlsTransport::outgoing(message_ptr message) {
	message->dscp = mCurrentDscp;

	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

bool DtlsTransport::demuxMessage(message_ptr) {
	// Dummy
	return false;
}

void DtlsTransport::postHandshake() {
	// Dummy
}

void DtlsTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;

	if (state() != State::Connecting && state() != State::Connected)
		return;

	try {
		const size_t bufferSize = 4096;
		char buffer[bufferSize];

		// Handle handshake if connecting
		if (state() == State::Connecting) {
			while (true) {
				int ret;
				{
					std::lock_guard lock(mSslMutex);
					ret = mbedtls_ssl_handshake(&mSsl);
				}

				if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
					auto timeout = mFinMs != 0 ? mTimerSetAt + milliseconds(mFinMs)
					                           : std::chrono::steady_clock::now() + milliseconds(MBEDTLS_SSL_DTLS_TIMEOUT_DFL_MIN);
					ThreadPool::Instance().schedule(timeout,
					                                [weak_this = weak_from_this()]() {
						                                if (auto locked = weak_this.lock())
							                                locked->doRecv();
					                                });
					return;
				}

				if (mbedtls::check(ret, "Handshake failed")) {
					// RFC 8261: DTLS MUST support sending messages larger than the current path MTU
					// See https://www.rfc-editor.org/rfc/rfc8261.html#section-5
					{
						std::lock_guard lock(mSslMutex);
						mbedtls_ssl_set_mtu(&mSsl, static_cast<unsigned int>(bufferSize + 1));
					}

					PLOG_INFO << "DTLS handshake finished";
					changeState(State::Connected);
					postHandshake();
					break;
				}
			}
		}

		if (state() == State::Connected) {
			while (true) {
				int ret;
				{
					std::lock_guard lock(mSslMutex);
					ret = mbedtls_ssl_read(&mSsl, reinterpret_cast<unsigned char *>(buffer),
					                       bufferSize);
				}

				if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
					return;
				}

				if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
					PLOG_DEBUG << "DTLS connection cleanly closed";
					break;
				}

				if (mbedtls::check(ret)) {
					if (ret == 0) {
						PLOG_DEBUG << "DTLS connection terminated";
						break;
					}
					auto *b = reinterpret_cast<byte *>(buffer);
					recv(make_message(b, b + ret));
				}
			}
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

int DtlsTransport::CertificateCallback(void *ctx, mbedtls_x509_crt *crt, int /*depth*/,
                                       uint32_t *flags) {
	auto this_ = static_cast<DtlsTransport *>(ctx);
	string fingerprint = make_fingerprint(crt, this_->mFingerprintAlgorithm);
	std::transform(fingerprint.begin(), fingerprint.end(), fingerprint.begin(),
	               [](char c) { return char(std::toupper(c)); });

	if (!this_->mVerifierCallback(fingerprint)) {
		if (flags) *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
		return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
	}

	return 0;
}

void DtlsTransport::ExportKeysCallback(void *ctx, mbedtls_ssl_key_export_type /*type*/,
                                       const unsigned char *secret, size_t secret_len,
                                       const unsigned char client_random[32],
                                       const unsigned char server_random[32],
                                       mbedtls_tls_prf_types tls_prf_type) {
	auto dtlsTransport = static_cast<DtlsTransport *>(ctx);
	std::memcpy(dtlsTransport->mMasterSecret, secret, secret_len);
	std::memcpy(dtlsTransport->mRandBytes, client_random, 32);
	std::memcpy(dtlsTransport->mRandBytes + 32, server_random, 32);
	dtlsTransport->mTlsProfile = tls_prf_type;
}

int DtlsTransport::WriteCallback(void *ctx, const unsigned char *buf, size_t len) {
	auto *t = static_cast<DtlsTransport *>(ctx);
	try {
		if (len > 0) {
			auto b = reinterpret_cast<const byte *>(buf);
			t->outgoing(make_message(b, b + len));
		}
		return int(len);

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
}

int DtlsTransport::ReadCallback(void *ctx, unsigned char *buf, size_t len) {
	auto *t = static_cast<DtlsTransport *>(ctx);
	try {
		while (t->mIncomingQueue.running()) {
			auto next = t->mIncomingQueue.pop();
			if (!next) {
				return MBEDTLS_ERR_SSL_WANT_READ;
			}

			message_ptr message = std::move(*next);
			if (t->demuxMessage(message))
				continue;

			auto bufMin = std::min(len, size_t(message->size()));
			std::memcpy(buf, message->data(), bufMin);
			return int(bufMin);
		}

		// Closed
		return 0;

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		;
	}
}

void DtlsTransport::SetTimerCallback(void *ctx, uint32_t int_ms, uint32_t fin_ms) {
	auto dtlsTransport = static_cast<DtlsTransport *>(ctx);
	dtlsTransport->mIntMs = int_ms;
	dtlsTransport->mFinMs = fin_ms;

	if (fin_ms != 0) {
		dtlsTransport->mTimerSetAt = std::chrono::steady_clock::now();
	}
}

int DtlsTransport::GetTimerCallback(void *ctx) {
	auto dtlsTransport = static_cast<DtlsTransport *>(ctx);
	auto now = std::chrono::steady_clock::now();

	if (dtlsTransport->mFinMs == 0) {
		return -1;
	} else if (now >= dtlsTransport->mTimerSetAt + milliseconds(dtlsTransport->mFinMs)) {
		return 2;
	} else if (now >= dtlsTransport->mTimerSetAt + milliseconds(dtlsTransport->mIntMs)) {
		return 1;
	} else {
		return 0;
	}
}

#elif USE_SCHANNEL

// Notes on the SChannel DTLS implementation (determined experimentally, as SChannel DTLS is mostly
// undocumented):
// - DTLS 1.2 requires Windows 10 or later. SCH_CREDENTIALS is rejected for DTLS, so the legacy
//   SCHANNEL_CRED structure is used.
// - Each output token is a single datagram. SEC_I_MESSAGE_FRAGMENT means more datagrams of the
//   same flight are pending: the function must be called again with an empty input token.
// - The path MTU must be passed as a SECBUFFER_DTLS_MTU input buffer on each call.
// - The use_srtp extension is negotiated with a SECBUFFER_SRTP_PROTECTION_PROFILES input buffer,
//   which must be passed on each call as well.
// - The server sends a HelloVerifyRequest and requires the client transport address as a
//   SECBUFFER_EXTRA input buffer to bind the cookie.
// - SChannel has no retransmission timer: calling the function with an empty input token
//   retransmits the last flight. However, once the handshake is finished, retransmissions from
//   the peer are not handled anymore, so the final flight is retransmitted manually.
// - Sending close_notify is not supported: ApplyControlToken(SCHANNEL_SHUTDOWN) followed by
//   InitializeSecurityContext/AcceptSecurityContext only retransmits the last flight.

namespace {

// SRTP protection profiles identifiers (RFC 5764 and RFC 7714)
const unsigned short SrtpAes128CmHmacSha1_80 = 0x0001;
const unsigned short SrtpAeadAes128Gcm = 0x0007;
const unsigned short SrtpAeadAes256Gcm = 0x0008;

// SEC_SRTP_PROTECTION_PROFILES with room for the profiles list
struct SrtpProtectionProfiles {
	unsigned short ProfilesSize; // in bytes
	unsigned short ProfilesList[3];
};

SrtpProtectionProfiles make_srtp_protection_profiles() {
	SrtpProtectionProfiles profiles = {};
	size_t count = 0;
#if RTC_ENABLE_MEDIA
	// AES-GCM is only available if libSRTP supports it, which is not the case of the bundled
	// libSRTP as it is built without third-party crypto library.
	if (DtlsSrtpTransport::IsGcmSupported()) {
		profiles.ProfilesList[count++] = SrtpAeadAes256Gcm;
		profiles.ProfilesList[count++] = SrtpAeadAes128Gcm;
	}
#endif
	// RFC 8827: The DTLS-SRTP protection profile SRTP_AES128_CM_HMAC_SHA1_80 MUST be supported
	// See https://www.rfc-editor.org/rfc/rfc8827.html#section-6.5
	profiles.ProfilesList[count++] = SrtpAes128CmHmacSha1_80;
	profiles.ProfilesSize = static_cast<unsigned short>(count * sizeof(unsigned short));
	return profiles;
}

// RFC 6347 recommends an initial timer value of 1s, doubled at each retransmission
const milliseconds InitialRetransmitTimeout = 1000ms;
const milliseconds MaxRetransmitTimeout = 60000ms;
// This allows for 5 retransmissions and fails after roughly 30s like the other backends
const milliseconds HandshakeTimeout = 30000ms;

// After the handshake, allow sending messages larger than the path MTU (RFC 8261)
const unsigned long ApplicationMtu = 16384; // Maximum DTLS record plaintext size

const unsigned long ClientRequirements =
    ISC_REQ_DATAGRAM | ISC_REQ_CONFIDENTIALITY | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_MANUAL_CRED_VALIDATION |
    ISC_REQ_USE_SUPPLIED_CREDS;

const unsigned long ServerRequirements =
    ASC_REQ_DATAGRAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
    ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_EXTENDED_ERROR | ASC_REQ_MUTUAL_AUTH;

// Opaque client address used by the server to bind the HelloVerifyRequest cookie. The transport
// path is already validated by ICE, so a constant value is used.
unsigned char ClientAddress[16] = {0x02, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x01};

} // namespace

void DtlsTransport::Init() {
	// Nothing to do
}

void DtlsTransport::Cleanup() {
	// Nothing to do
}

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
                             optional<size_t> mtu,
                             CertificateFingerprint::Algorithm fingerprintAlgorithm,
                             verifier_callback verifierCallback, state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mMtu(mtu),
      mCertificate(std::move(certificate)), mFingerprintAlgorithm(fingerprintAlgorithm),
      mVerifierCallback(std::move(verifierCallback)),
      mIsClient(lower->role() == Description::Role::Active),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing DTLS transport (SChannel)";

	if (!mCertificate)
		throw std::invalid_argument("DTLS certificate is null");

	PCCERT_CONTEXT cert = mCertificate->credentials();

	SCHANNEL_CRED cred = {};
	cred.dwVersion = SCHANNEL_CRED_VERSION;
	cred.cCreds = 1;
	cred.paCred = &cert;
	cred.grbitEnabledProtocols = mIsClient ? SP_PROT_DTLS1_2_CLIENT : SP_PROT_DTLS1_2_SERVER;
	// Certificates are self-signed in WebRTC, so chain validation is disabled and the peer
	// certificate fingerprint is verified instead
	cred.dwFlags =
	    SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;

	TimeStamp expiry;
	schannel::check(
	    AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
	                              mIsClient ? SECPKG_CRED_OUTBOUND : SECPKG_CRED_INBOUND, nullptr,
	                              &cred, nullptr, nullptr, mCredentials.get(), &expiry),
	    "Failed to acquire DTLS credentials (DTLS 1.2 requires Windows 10 or later)");

	// Set recommended medium-priority DSCP value for handshake
	// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
	mCurrentDscp = 10; // AF11: Assured Forwarding class 1, low drop probability
}

DtlsTransport::~DtlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying DTLS transport";
}

void DtlsTransport::start() {
	PLOG_DEBUG << "Starting DTLS transport";

	PLOG_VERBOSE << "DTLS MTU set to " << (mMtu.value_or(DEFAULT_MTU) - 8 - 40); // UDP/IPv6
	mHandshakeStartTime = std::chrono::steady_clock::now();

	registerIncoming();
	changeState(State::Connecting);

	enqueueRecv(); // to initiate the handshake
}

void DtlsTransport::stop() {
	PLOG_DEBUG << "Stopping DTLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
}

bool DtlsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();

	{
		std::lock_guard lock(mSslMutex);
		if (message->size() > size_t(mStreamSizes.cbMaximumMessage))
			return false;

		const size_t headerSize = mStreamSizes.cbHeader;
		const size_t trailerSize = mStreamSizes.cbTrailer;
		binary buffer(headerSize + message->size() + trailerSize);
		std::memcpy(buffer.data() + headerSize, message->data(), message->size());

		SecBuffer buffers[4] = {};
		buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
		buffers[0].pvBuffer = buffer.data();
		buffers[0].cbBuffer = ULONG(headerSize);
		buffers[1].BufferType = SECBUFFER_DATA;
		buffers[1].pvBuffer = buffer.data() + headerSize;
		buffers[1].cbBuffer = ULONG(message->size());
		buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
		buffers[2].pvBuffer = buffer.data() + headerSize + message->size();
		buffers[2].cbBuffer = ULONG(trailerSize);
		buffers[3].BufferType = SECBUFFER_EMPTY;
		SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};

		schannel::check(EncryptMessage(mContext.get(), 0, &desc, 0), "DTLS encryption failed");

		// Header and data stay contiguous, but the trailer size may change
		buffer.resize(buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);

		mCurrentDscp = message->dscp;
		outgoing(make_message(buffer.begin(), buffer.end()));
	}

	return mOutgoingResult;
}

void DtlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		enqueueRecv();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	if (mIncomingQueue.tryPush(std::move(message))) {
		enqueueRecv();
	} else {
		PLOG_VERBOSE << "DTLS incoming queue is full, dropping";
	}
}

bool DtlsTransport::outgoing(message_ptr message) {
	message->dscp = mCurrentDscp;

	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

bool DtlsTransport::demuxMessage(message_ptr) {
	// Dummy
	return false;
}

void DtlsTransport::postHandshake() {
	// Dummy
}

void DtlsTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;

	if (state() != State::Connecting && state() != State::Connected)
		return;

	try {
		if (state() == State::Connecting && mIsClient) {
			std::lock_guard lock(mSslMutex);
			if (!mContext.valid()) {
				// Initiate the handshake
				binary remaining;
				bool sent = false;
				handshakeStep({}, remaining, sent);
				if (sent)
					armRetransmitTimer(true);
			}
		}

		// Process pending messages
		while (mIncomingQueue.running()) {
			auto next = mIncomingQueue.pop();
			if (!next) {
				// No more messages pending, handle timeout if connecting
				if (state() == State::Connecting)
					handleTimeout();

				return;
			}

			message_ptr message = std::move(*next);
			if (demuxMessage(message))
				continue;

			if (state() == State::Connecting) {
				// Continue the handshake
				binary remaining;
				bool finished;
				{
					std::lock_guard lock(mSslMutex);
					bool sent = false;
					finished =
					    handshakeStep(binary(message->begin(), message->end()), remaining, sent);
					if (!finished && sent)
						armRetransmitTimer(true);
				}

				if (!finished)
					continue;

				completeHandshake();

				PLOG_INFO << "DTLS handshake finished";
				postHandshake();
				changeState(State::Connected);

				if (remaining.empty())
					continue;

				// Process application data received in the same datagram
				message = make_message(remaining.begin(), remaining.end());
			}

			if (state() == State::Connected) {
				std::vector<message_ptr> received;
				bool closed = decryptDatagram(std::move(message), received);
				for (auto &m : received)
					recv(std::move(m));

				if (closed) {
					PLOG_DEBUG << "DTLS connection cleanly closed";
					break;
				}
			}
		}

		// SChannel can't send close_notify for DTLS, see notes above

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

bool DtlsTransport::handshakeStep(binary input, binary &remaining, bool &sent) {
	// Requires mSslMutex to be locked
	SrtpProtectionProfiles profiles = make_srtp_protection_profiles();
	SEC_DTLS_MTU mtu = {};
	mtu.PathMTU = static_cast<unsigned short>(mMtu.value_or(DEFAULT_MTU) - 8 - 40); // UDP/IPv6

	while (true) {
		SecBuffer in[5] = {};
		ULONG count = 0;
		in[count].BufferType = SECBUFFER_TOKEN;
		in[count].pvBuffer = input.empty() ? nullptr : input.data();
		in[count].cbBuffer = ULONG(input.size());
		++count;
		in[count].BufferType = SECBUFFER_EMPTY; // receives SECBUFFER_EXTRA if any
		++count;
		in[count].BufferType = SECBUFFER_SRTP_PROTECTION_PROFILES;
		in[count].pvBuffer = &profiles;
		in[count].cbBuffer = ULONG(sizeof(profiles.ProfilesSize) + profiles.ProfilesSize);
		++count;
		in[count].BufferType = SECBUFFER_DTLS_MTU;
		in[count].pvBuffer = &mtu;
		in[count].cbBuffer = ULONG(sizeof(mtu));
		++count;
		if (!mIsClient) {
			in[count].BufferType = SECBUFFER_EXTRA;
			in[count].pvBuffer = ClientAddress;
			in[count].cbBuffer = ULONG(sizeof(ClientAddress));
			++count;
		}
		SecBufferDesc inDesc = {SECBUFFER_VERSION, count, in};

		schannel::ContextBuffers out({SECBUFFER_TOKEN, SECBUFFER_ALERT, SECBUFFER_EMPTY});

		const bool hadContext = mContext.valid();
		ULONG attributes = 0;
		SECURITY_STATUS status;
		if (mIsClient)
			status = InitializeSecurityContextW(mCredentials.get(), mContext.getOrNull(), nullptr,
			                                    ClientRequirements, 0, 0, &inDesc, 0,
			                                    mContext.get(), out.desc(), &attributes, nullptr);
		else
			status = AcceptSecurityContext(mCredentials.get(), mContext.getOrNull(), &inDesc,
			                               ServerRequirements, 0, mContext.get(), out.desc(),
			                               &attributes, nullptr);

		if (FAILED(status) && !hadContext)
			SecInvalidateHandle(mContext.get());

		// Verify the peer certificate as soon as possible, before sending the next flight
		if (!mPeerVerified && (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED ||
		                       status == SEC_I_MESSAGE_FRAGMENT))
			verifyPeerCertificate(status == SEC_E_OK);

		if (const SecBuffer *token = out.find(SECBUFFER_TOKEN)) {
			auto data = static_cast<const byte *>(token->pvBuffer);
			if (!sent) {
				mLastFlight.clear();
				sent = true;
			}
			mLastFlight.emplace_back(data, data + token->cbBuffer);
			outgoing(make_message(data, data + token->cbBuffer));
		}

		switch (status) {
		case SEC_I_MESSAGE_FRAGMENT:
			// More datagrams of the same flight are pending
			input.clear();
			continue;

		case SEC_I_CONTINUE_NEEDED:
		case SEC_E_OK: {
			binary extra;
			if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0 &&
			    in[1].cbBuffer <= input.size())
				extra.assign(input.end() - in[1].cbBuffer, input.end());

			if (status == SEC_E_OK) {
				// Keep the final flight in case the peer did not receive it
				if (sent)
					mFinalFlight = mLastFlight;
				else
					mFinalFlight.clear();

				mLastFlight.clear();
				mRetransmitArmed = false;
				remaining = std::move(extra);
				return true;
			}

			if (extra.empty())
				return false;

			input = std::move(extra);
			continue;
		}

		case SEC_E_INCOMPLETE_MESSAGE:
			// Happens for instance on retransmission timeout on the server side before the cookie
			// exchange, as the server is stateless at this point
			return false;

		default:
			if (const SecBuffer *alert = out.find(SECBUFFER_ALERT)) {
				auto data = static_cast<const uint8_t *>(alert->pvBuffer);
				if (alert->cbBuffer >= 2)
					PLOG_WARNING << "DTLS alert generated: level=" << unsigned(data[0])
					             << ", description=" << unsigned(data[1]);
			}
			throw std::runtime_error("Handshake failed: " + schannel::error_string(status));
		}
	}
}

void DtlsTransport::verifyPeerCertificate(bool required) {
	// Requires mSslMutex to be locked
	PCCERT_CONTEXT remote = nullptr;
	SECURITY_STATUS status =
	    QueryContextAttributesW(mContext.get(), SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote);
	if (status != SEC_E_OK || !remote) {
		if (required)
			throw std::runtime_error("DTLS peer certificate is missing");

		return; // not available yet
	}

	auto cert = schannel::make_cert_context_ptr(remote);
	string fingerprint = make_fingerprint(cert.get(), mFingerprintAlgorithm);
	if (!mVerifierCallback(fingerprint))
		throw std::runtime_error("DTLS peer certificate verification failed");

	mPeerVerified = true;
}

void DtlsTransport::completeHandshake() {
	std::lock_guard lock(mSslMutex);

	// RFC 8261: DTLS MUST support sending messages larger than the current path MTU
	// See https://www.rfc-editor.org/rfc/rfc8261.html#section-5
	unsigned long mtu = ApplicationMtu;
	SECURITY_STATUS status =
	    SetContextAttributesW(mContext.get(), SECPKG_ATTR_DTLS_MTU, &mtu, sizeof(mtu));
	if (status != SEC_E_OK)
		PLOG_WARNING << "Failed to set DTLS MTU: " << schannel::error_string(status);

	schannel::check(
	    QueryContextAttributesW(mContext.get(), SECPKG_ATTR_STREAM_SIZES, &mStreamSizes),
	    "Failed to query DTLS sizes");

	SecPkgContext_ConnectionInfo info = {};
	if (QueryContextAttributesW(mContext.get(), SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK)
		PLOG_DEBUG << "DTLS protocol=0x" << std::hex << info.dwProtocol << ", cipher=0x"
		           << info.aiCipher << ", exchange=0x" << info.aiExch << std::dec
		           << ", max message size=" << mStreamSizes.cbMaximumMessage;
}

void DtlsTransport::handleTimeout() {
	std::lock_guard lock(mSslMutex);
	if (!mRetransmitArmed)
		return;

	auto now = std::chrono::steady_clock::now();
	if (now < mRetransmitTime)
		return; // a timer is already scheduled

	if (now - mHandshakeStartTime > HandshakeTimeout)
		throw std::runtime_error("Handshake timeout");

	// SChannel retransmits the last flight when called with an empty input token
	binary remaining;
	bool sent = false;
	handshakeStep({}, remaining, sent);
	if (sent)
		PLOG_VERBOSE << "DTLS retransmit done";

	armRetransmitTimer(false);
}

void DtlsTransport::armRetransmitTimer(bool reset) {
	// Requires mSslMutex to be locked
	mRetransmitTimeout =
	    reset ? InitialRetransmitTimeout : std::min(mRetransmitTimeout * 2, MaxRetransmitTimeout);
	mRetransmitTime = std::chrono::steady_clock::now() + mRetransmitTimeout;
	mRetransmitArmed = true;

	PLOG_VERBOSE << "DTLS retransmit timeout is " << mRetransmitTimeout.count() << "ms";
	ThreadPool::Instance().schedule(mRetransmitTimeout, [weak_this = weak_from_this()]() {
		if (auto locked = weak_this.lock())
			locked->doRecv();
	});
}

bool DtlsTransport::decryptDatagram(message_ptr message, std::vector<message_ptr> &received) {
	std::lock_guard lock(mSslMutex);

	binary data(message->begin(), message->end());
	const bool isHandshake = !data.empty() && std::to_integer<uint8_t>(data[0]) == 22;

	while (!data.empty()) {
		SecBuffer buffers[4] = {};
		buffers[0].BufferType = SECBUFFER_DATA;
		buffers[0].pvBuffer = data.data();
		buffers[0].cbBuffer = ULONG(data.size());
		buffers[1].BufferType = SECBUFFER_EMPTY;
		buffers[2].BufferType = SECBUFFER_EMPTY;
		buffers[3].BufferType = SECBUFFER_EMPTY;
		SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};

		SECURITY_STATUS status = DecryptMessage(mContext.get(), &desc, 0, nullptr);

		if (status == SEC_I_CONTEXT_EXPIRED)
			return true; // close_notify received

		if (status != SEC_E_OK) {
			if (isHandshake && !mFinalFlight.empty()) {
				// The peer retransmitted its last flight, so it did not receive our final flight.
				// SChannel does not handle it once the handshake is finished.
				PLOG_VERBOSE << "Retransmitting final DTLS handshake flight";
				for (const auto &d : mFinalFlight)
					outgoing(make_message(d.begin(), d.end()));
			} else {
				// RFC 6347: Invalid records SHOULD be silently discarded
				PLOG_VERBOSE << "Discarding DTLS record: " << schannel::error_string(status);
			}
			return false;
		}

		// The peer finished the handshake
		mFinalFlight.clear();

		binary extra;
		for (const auto &buffer : buffers) {
			if (!buffer.pvBuffer || buffer.cbBuffer == 0)
				continue;

			auto p = static_cast<const byte *>(buffer.pvBuffer);
			if (buffer.BufferType == SECBUFFER_DATA)
				received.push_back(make_message(p, p + buffer.cbBuffer));
			else if (buffer.BufferType == SECBUFFER_EXTRA)
				extra.assign(p, p + buffer.cbBuffer);
		}

		data = std::move(extra);
	}

	return false;
}

#else // OPENSSL

BIO_METHOD *DtlsTransport::BioMethods = nullptr;
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
		TransportExIndex = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
	}
}

void DtlsTransport::Cleanup() {
	// Nothing to do
}

DtlsTransport::DtlsTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
                             optional<size_t> mtu,
                             CertificateFingerprint::Algorithm fingerprintAlgorithm,
                             verifier_callback verifierCallback, state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mMtu(mtu), mCertificate(std::move(certificate)),
      mFingerprintAlgorithm(fingerprintAlgorithm), mVerifierCallback(std::move(verifierCallback)),
      mIsClient(lower->role() == Description::Role::Active),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing DTLS transport (OpenSSL)";

	if (!mCertificate)
		throw std::invalid_argument("DTLS certificate is null");

	try {
		mCtx = SSL_CTX_new(DTLS_method());
		if (!mCtx)
			throw std::runtime_error("Failed to create SSL context");

		// RFC 8261: SCTP performs segmentation and reassembly based on the path MTU.
		// Therefore, the DTLS layer MUST NOT use any compression algorithm.
		// See https://www.rfc-editor.org/rfc/rfc8261.html#section-5
		// RFC 8827: Implementations MUST NOT implement DTLS renegotiation
		// See https://www.rfc-editor.org/rfc/rfc8827.html#section-6.5
		SSL_CTX_set_options(mCtx, SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_NO_QUERY_MTU |
		                              SSL_OP_NO_RENEGOTIATION);

		SSL_CTX_set_min_proto_version(mCtx, DTLS1_VERSION);
		SSL_CTX_set_read_ahead(mCtx, 1);
		SSL_CTX_set_quiet_shutdown(mCtx, 0); // send the close_notify alert
		SSL_CTX_set_info_callback(mCtx, InfoCallback);

		SSL_CTX_set_verify(mCtx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
		                   CertificateCallback);
		SSL_CTX_set_verify_depth(mCtx, 1);

		openssl::check(SSL_CTX_set_cipher_list(mCtx, "ALL:!SHA256:!SHA384:!aPSK:!ECDSA+SHA1:!ADH:!LOW:!EXP:!MD5:!3DES:!SSLv3:!TLSv1"),
		               "Failed to set SSL priorities");

#if OPENSSL_VERSION_NUMBER >= 0x30000000
		openssl::check(SSL_CTX_set1_groups_list(mCtx, "P-256"), "Failed to set SSL groups");
#else
		auto ecdh = unique_ptr<EC_KEY, decltype(&EC_KEY_free)>(
		    EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
		SSL_CTX_set_tmp_ecdh(mCtx, ecdh.get());
#endif

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

		// RFC 8827: The DTLS-SRTP protection profile SRTP_AES128_CM_HMAC_SHA1_80 MUST be supported
		// See https://www.rfc-editor.org/rfc/rfc8827.html#section-6.5
		// Warning: SSL_set_tlsext_use_srtp() returns 0 on success and 1 on error
#if RTC_ENABLE_MEDIA
		// Try to use GCM suite
		if (!DtlsSrtpTransport::IsGcmSupported() ||
		    SSL_set_tlsext_use_srtp(
		        mSsl, "SRTP_AEAD_AES_256_GCM:SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80")) {
			PLOG_WARNING << "AES-GCM for SRTP is not supported, falling back to default profile";
			if (SSL_set_tlsext_use_srtp(mSsl, "SRTP_AES128_CM_SHA1_80"))
				throw std::runtime_error("Failed to set SRTP profile: " +
				                         openssl::error_string(ERR_get_error()));
		}
#else
		if (SSL_set_tlsext_use_srtp(mSsl, "SRTP_AES128_CM_SHA1_80"))
			throw std::runtime_error("Failed to set SRTP profile: " +
			                         openssl::error_string(ERR_get_error()));
#endif
	} catch (...) {
		if (mSsl)
			SSL_free(mSsl);
		if (mCtx)
			SSL_CTX_free(mCtx);
		throw;
	}

	// Set recommended medium-priority DSCP value for handshake
	// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
	mCurrentDscp = 10; // AF11: Assured Forwarding class 1, low drop probability
}

DtlsTransport::~DtlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying DTLS transport";
	SSL_free(mSsl);
	SSL_CTX_free(mCtx);
}

void DtlsTransport::start() {
	PLOG_DEBUG << "Starting DTLS transport";

	// Set the MTU
	{
		std::lock_guard lock(mSslMutex);
		size_t mtu = mMtu.value_or(DEFAULT_MTU) - 8 - 40; // UDP/IPv6
		SSL_set_mtu(mSsl, static_cast<unsigned int>(mtu));
		PLOG_VERBOSE << "DTLS MTU set to " << mtu;
	}

	registerIncoming();
	changeState(State::Connecting);

	// Initiate the handshake
	int ret, err;
	{
		std::lock_guard lock(mSslMutex);
		ret = SSL_do_handshake(mSsl);
		err = SSL_get_error(mSsl, ret);
	}
	openssl::check_error(err, "Handshake failed", mSsl);

	// start() can run on the libjuice poll thread via the ICE state-change
	// callback, while juice still holds its registry mutex. Calling
	// handleTimeout() inline then waits on mSslMutex, which a thread pool
	// recv thread may hold while blocked in juice_send (conn_lock) on the
	// TURN relay path, forming an ABBA deadlock. Defer the initial timeout
	// handling to the thread pool, as the other TLS backends do.
	enqueueRecv(); // to initiate the handshake
}

void DtlsTransport::stop() {
	PLOG_DEBUG << "Stopping DTLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
}

bool DtlsTransport::send(message_ptr message) {
	if (!message || state() != State::Connected)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();

	int ret, err;
	{
		std::lock_guard lock(mSslMutex);
		mCurrentDscp = message->dscp;
		ret = SSL_write(mSsl, message->data(), int(message->size()));
		err = SSL_get_error(mSsl, ret);
	}

	if (!openssl::check_error(err))
		return false;

	return mOutgoingResult;
}

void DtlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		enqueueRecv();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	if (mIncomingQueue.tryPush(std::move(message))) {
		enqueueRecv();
	} else {
		PLOG_VERBOSE << "DTLS incoming queue is full, dropping";
	}
}

bool DtlsTransport::outgoing(message_ptr message) {
	message->dscp = mCurrentDscp;

	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

bool DtlsTransport::demuxMessage(message_ptr) {
	// Dummy
	return false;
}

void DtlsTransport::postHandshake() {
	// Dummy
}

void DtlsTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;

	if (state() != State::Connecting && state() != State::Connected)
		return;

	try {
		const size_t bufferSize = 4096;
		byte buffer[bufferSize];

		// Process pending messages
		while (mIncomingQueue.running()) {
			auto next = mIncomingQueue.pop();
			if (!next) {
				// No more messages pending, handle timeout if connecting
				if (state() == State::Connecting)
					handleTimeout();

				return;
			}

			message_ptr message = std::move(*next);
			if (demuxMessage(message))
				continue;

			bool incomingWritten = false;
			if (state() == State::Connecting) {
				// Continue the handshake
				int ret, err;
				{
					std::lock_guard lock(mSslMutex);
					BIO_write(mInBio, message->data(), int(message->size()));
					incomingWritten = true;
					ret = SSL_do_handshake(mSsl);
					err = SSL_get_error(mSsl, ret);
				}

				if (openssl::check_error(err, "Handshake failed", mSsl)) {
					// RFC 8261: DTLS MUST support sending messages larger than the current path MTU
					// See https://www.rfc-editor.org/rfc/rfc8261.html#section-5
					{
						std::lock_guard lock(mSslMutex);
						SSL_set_mtu(mSsl, bufferSize + 1);
					}

					PLOG_INFO << "DTLS handshake finished";
					postHandshake();
					changeState(State::Connected);
				}
			}

			if (state() == State::Connected) {
				int ret, err;
				{
					std::lock_guard lock(mSslMutex);
					if (!incomingWritten)
						BIO_write(mInBio, message->data(), int(message->size()));
					ret = SSL_read(mSsl, buffer, bufferSize);
					err = SSL_get_error(mSsl, ret);
				}

				if (err == SSL_ERROR_ZERO_RETURN) {
					PLOG_DEBUG << "TLS connection cleanly closed";
					break;
				}

				if (openssl::check_error(err))
					recv(make_message(buffer, buffer + ret));
			}
		}

		std::lock_guard lock(mSslMutex);
		SSL_shutdown(mSsl);

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

void DtlsTransport::handleTimeout() {
	std::lock_guard lock(mSslMutex);

	// Warning: This function breaks the usual return value convention
	int ret = DTLSv1_handle_timeout(mSsl);
	if (ret < 0) {
		throw std::runtime_error("Handshake timeout"); // write BIO can't fail
	} else if (ret > 0) {
		LOG_VERBOSE << "DTLS retransmit done";
	}

	struct timeval tv = {};
	if (DTLSv1_get_timeout(mSsl, &tv)) {
		auto timeout = milliseconds(tv.tv_sec * 1000 + tv.tv_usec / 1000);
		// Also handle handshake timeout manually because OpenSSL actually
		// doesn't... OpenSSL backs off exponentially in base 2 starting from the
		// recommended 1s so this allows for 5 retransmissions and fails after
		// roughly 30s.
		if (timeout > 30s)
			throw std::runtime_error("Handshake timeout");

		LOG_VERBOSE << "DTLS retransmit timeout is " << timeout.count() << "ms";
		ThreadPool::Instance().schedule(timeout, [weak_this = weak_from_this()]() {
			if (auto locked = weak_this.lock())
				locked->doRecv();
		});
	}
}

int DtlsTransport::CertificateCallback(int /*preverify_ok*/, X509_STORE_CTX *ctx) {
	SSL *ssl =
	    static_cast<SSL *>(X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
	auto *t =
	    static_cast<DtlsTransport *>(SSL_get_ex_data(ssl, DtlsTransport::TransportExIndex));

	X509 *crt = X509_STORE_CTX_get_current_cert(ctx);
	string fingerprint = make_fingerprint(crt, t->mFingerprintAlgorithm);

	return t->mVerifierCallback(fingerprint) ? 1 : 0;
}

void DtlsTransport::InfoCallback(const SSL *ssl, int where, int ret) {
	auto *t =
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
	BIO_set_data(bio, nullptr);
	BIO_set_shutdown(bio, 0);
	return 1;
}

int DtlsTransport::BioMethodFree(BIO *bio) {
	if (!bio)
		return 0;
	BIO_set_data(bio, nullptr);
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
