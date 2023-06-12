/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tlstransport.hpp"
#include "tcptransport.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>

using namespace std::chrono;

namespace rtc::impl {

#if USE_GNUTLS

namespace {

gnutls_certificate_credentials_t default_certificate_credentials() {
	static std::mutex mutex;
	static shared_ptr<gnutls_certificate_credentials_t> creds;

	std::lock_guard lock(mutex);
	if (!creds) {
		creds = shared_ptr<gnutls_certificate_credentials_t>(gnutls::new_credentials(),
		                                                     gnutls::free_credentials);
		gnutls::check(gnutls_certificate_set_x509_system_trust(*creds));
	}
	return *creds;
}

} // namespace

void TlsTransport::Init() {
	// Nothing to do
}

void TlsTransport::Cleanup() {
	// Nothing to do
}

TlsTransport::TlsTransport(shared_ptr<TcpTransport> lower, optional<string> host,
                           certificate_ptr certificate, state_callback callback)
    : Transport(lower, std::move(callback)), mHost(std::move(host)), mIsClient(lower->isActive()),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing TLS transport (GnuTLS)";

	gnutls::check(gnutls_init(&mSession, mIsClient ? GNUTLS_CLIENT : GNUTLS_SERVER));

	try {
		const char *priorities = "SECURE128:-VERS-SSL3.0:-ARCFOUR-128";
		const char *err_pos = NULL;
		gnutls::check(gnutls_priority_set_direct(mSession, priorities, &err_pos),
		              "Failed to set TLS priorities");

		gnutls::check(gnutls_credentials_set(mSession, GNUTLS_CRD_CERTIFICATE,
		                                     certificate ? certificate->credentials()
		                                                 : default_certificate_credentials()));

		if (mIsClient && mHost) {
			PLOG_VERBOSE << "Server Name Indication: " << *mHost;
			gnutls_server_name_set(mSession, GNUTLS_NAME_DNS, mHost->data(), mHost->size());
		}

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

TlsTransport::~TlsTransport() {
	stop();
	gnutls_deinit(mSession);
}

void TlsTransport::start() {
	if (mStarted.exchange(true))
		return;

	PLOG_DEBUG << "Starting TLS recv thread";
	registerIncoming();
	mRecvThread = std::thread(&TlsTransport::runRecvLoop, this);
}

void TlsTransport::stop() {
	if (!mStarted.exchange(false))
		return;

	PLOG_DEBUG << "Stopping TLS recv thread";
	unregisterIncoming();
	mIncomingQueue.stop();
	mRecvThread.join();
}

bool TlsTransport::send(message_ptr message) {
	if (state() != State::Connected)
		throw std::runtime_error("TLS is not open");

	if (!message || message->size() == 0)
		return outgoing(message); // pass through

	PLOG_VERBOSE << "Send size=" << message->size();

	ssize_t ret;
	do {
		ret = gnutls_record_send(mSession, message->data(), message->size());
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	if (!gnutls::check(ret))
		throw std::runtime_error("TLS send failed");

	return mOutgoingResult;
}

void TlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
}

bool TlsTransport::outgoing(message_ptr message) {
	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

void TlsTransport::postHandshake() {
	// Dummy
}

void TlsTransport::runRecvLoop() {
	const size_t bufferSize = 4096;
	char buffer[bufferSize];

	// Handshake loop
	try {
		changeState(State::Connecting);

		int ret;
		do {
			ret = gnutls_handshake(mSession);
		} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN ||
		         !gnutls::check(ret, "TLS handshake failed"));

	} catch (const std::exception &e) {
		PLOG_ERROR << "TLS handshake: " << e.what();
		changeState(State::Failed);
		return;
	}

	// Receive loop
	try {
		PLOG_INFO << "TLS handshake finished";
		changeState(State::Connected);
		postHandshake();

		while (true) {
			ssize_t ret;
			do {
				ret = gnutls_record_recv(mSession, buffer, bufferSize);
			} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

			// Consider premature termination as remote closing
			if (ret == GNUTLS_E_PREMATURE_TERMINATION) {
				PLOG_DEBUG << "TLS connection terminated";
				break;
			}

			if (gnutls::check(ret)) {
				if (ret == 0) {
					// Closed
					PLOG_DEBUG << "TLS connection cleanly closed";
					break;
				}
				auto *b = reinterpret_cast<byte *>(buffer);
				recv(make_message(b, b + ret));
			}
		}
	} catch (const std::exception &e) {
		PLOG_ERROR << "TLS recv: " << e.what();
	}

	gnutls_bye(mSession, GNUTLS_SHUT_WR);

	PLOG_INFO << "TLS closed";
	changeState(State::Disconnected);
	recv(nullptr);
}

ssize_t TlsTransport::WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len) {
	TlsTransport *t = static_cast<TlsTransport *>(ptr);
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

ssize_t TlsTransport::ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen) {
	TlsTransport *t = static_cast<TlsTransport *>(ptr);
	try {
		message_ptr &message = t->mIncomingMessage;
		size_t &position = t->mIncomingMessagePosition;

		if (message && position >= message->size())
			message.reset();

		if (!message) {
			position = 0;
			while (auto next = t->mIncomingQueue.pop()) {
				message = *next;
				if (message->size() > 0)
					break;
				else
					t->recv(message); // Pass zero-sized messages through
			}
		}

		if (message) {
			size_t available = message->size() - position;
			ssize_t len = std::min(maxlen, available);
			std::memcpy(data, message->data() + position, len);
			position += len;
			gnutls_transport_set_errno(t->mSession, 0);
			return len;
		} else {
			// Closed
			gnutls_transport_set_errno(t->mSession, 0);
			return 0;
		}

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		gnutls_transport_set_errno(t->mSession, ECONNRESET);
		return -1;
	}
}

int TlsTransport::TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms) {
	TlsTransport *t = static_cast<TlsTransport *>(ptr);
	try {
		message_ptr &message = t->mIncomingMessage;
		size_t &position = t->mIncomingMessagePosition;

		if(message && position < message->size())
			return 1;

		bool isReadable = t->mIncomingQueue.wait(
		    ms != GNUTLS_INDEFINITE_TIMEOUT ? std::make_optional(milliseconds(ms)) : nullopt);
		return isReadable ? 1 : 0;

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return 1;
	}
}

#else // USE_GNUTLS==0

int TlsTransport::TransportExIndex = -1;

void TlsTransport::Init() {
	openssl::init();

	if (TransportExIndex < 0) {
		TransportExIndex = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	}
}

void TlsTransport::Cleanup() {
	// Nothing to do
}

TlsTransport::TlsTransport(shared_ptr<TcpTransport> lower, optional<string> host,
                           certificate_ptr certificate, state_callback callback)
    : Transport(lower, std::move(callback)), mHost(std::move(host)), mIsClient(lower->isActive()),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing TLS transport (OpenSSL)";

	try {
		if (!(mCtx = SSL_CTX_new(SSLv23_method()))) // version-flexible
			throw std::runtime_error("Failed to create SSL context");

		openssl::check(SSL_CTX_set_cipher_list(mCtx, "ALL:!LOW:!EXP:!RC4:!MD5:@STRENGTH"),
		               "Failed to set SSL priorities");

		if (certificate) {
			auto [x509, pkey] = certificate->credentials();
			SSL_CTX_use_certificate(mCtx, x509);
			SSL_CTX_use_PrivateKey(mCtx, pkey);
		} else {
			if (!SSL_CTX_set_default_verify_paths(mCtx)) {
				PLOG_WARNING << "SSL root CA certificates unavailable";
			}
		}

		SSL_CTX_set_options(mCtx, SSL_OP_NO_SSLv3 | SSL_OP_NO_RENEGOTIATION);
		SSL_CTX_set_min_proto_version(mCtx, TLS1_VERSION);
		SSL_CTX_set_read_ahead(mCtx, 1);
		SSL_CTX_set_quiet_shutdown(mCtx, 0); // send the close_notify alert
		SSL_CTX_set_info_callback(mCtx, InfoCallback);
		SSL_CTX_set_verify(mCtx, SSL_VERIFY_NONE, NULL);

		if (!(mSsl = SSL_new(mCtx)))
			throw std::runtime_error("Failed to create SSL instance");

		SSL_set_ex_data(mSsl, TransportExIndex, this);

		if (mIsClient && mHost) {
			SSL_set_hostflags(mSsl, 0);
			openssl::check(SSL_set1_host(mSsl, mHost->c_str()), "Failed to set SSL host");

			PLOG_VERBOSE << "Server Name Indication: " << *mHost;
			SSL_set_tlsext_host_name(mSsl, mHost->c_str());
		}

		if (mIsClient)
			SSL_set_connect_state(mSsl);
		else
			SSL_set_accept_state(mSsl);

		if (!(mInBio = BIO_new(BIO_s_mem())) || !(mOutBio = BIO_new(BIO_s_mem())))
			throw std::runtime_error("Failed to create BIO");

		BIO_set_mem_eof_return(mInBio, BIO_EOF);
		BIO_set_mem_eof_return(mOutBio, BIO_EOF);
		SSL_set_bio(mSsl, mInBio, mOutBio);

		auto ecdh = unique_ptr<EC_KEY, decltype(&EC_KEY_free)>(
		    EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
		SSL_set_options(mSsl, SSL_OP_SINGLE_ECDH_USE);
		SSL_set_tmp_ecdh(mSsl, ecdh.get());

	} catch (...) {
		if (mSsl)
			SSL_free(mSsl);
		if (mCtx)
			SSL_CTX_free(mCtx);
		throw;
	}
}

TlsTransport::~TlsTransport() {
	stop();
	SSL_free(mSsl);
	SSL_CTX_free(mCtx);
}

void TlsTransport::start() {
	if (mStarted.exchange(true))
		return;

	PLOG_DEBUG << "Starting TLS recv thread";
	registerIncoming();
	mRecvThread = std::thread(&TlsTransport::runRecvLoop, this);
}

void TlsTransport::stop() {
	if (!mStarted.exchange(false))
		return;

	PLOG_DEBUG << "Stopping TLS recv thread";
	unregisterIncoming();
	mIncomingQueue.stop();
	mRecvThread.join();
}

bool TlsTransport::send(message_ptr message) {
	if (state() != State::Connected)
		throw std::runtime_error("TLS is not open");

	if (!message || message->size() == 0)
		return outgoing(message); // pass through

	PLOG_VERBOSE << "Send size=" << message->size();

	std::lock_guard lock(mSslMutex);
	int ret = SSL_write(mSsl, message->data(), int(message->size()));
	if (!openssl::check(mSsl, ret))
		throw std::runtime_error("TLS send failed");

	return flushOutput();
}

void TlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
}

bool TlsTransport::outgoing(message_ptr message) { return Transport::outgoing(std::move(message)); }

void TlsTransport::postHandshake() {
	// Dummy
}

void TlsTransport::runRecvLoop() {
	const size_t bufferSize = 4096;
	byte buffer[bufferSize];

	try {
		changeState(State::Connecting);

		while (true) {
			if (state() == State::Connecting) {
				// Initiate or continue the handshake
				bool finished;
				{
					std::lock_guard lock(mSslMutex);
					int ret = SSL_do_handshake(mSsl);
					if (!openssl::check(mSsl, ret, "Handshake failed"))
						break;

					flushOutput();
					finished = (SSL_is_init_finished(mSsl) != 0);
				}

				if (finished) {
					PLOG_INFO << "TLS handshake finished";
					changeState(State::Connected);
					postHandshake();
				}
			}

			if (state() == State::Connected) {
				int ret;
				{
					std::lock_guard lock(mSslMutex);
					ret = SSL_read(mSsl, buffer, bufferSize);
					if (!openssl::check(mSsl, ret))
						break;

					flushOutput(); // SSL_read() can also cause write operations
				}

				if (ret > 0) {
					recv(make_message(buffer, buffer + ret));
					continue;
				}
			}

			auto next = mIncomingQueue.pop();
			if (!next)
				break;

			message_ptr message = std::move(*next);
			if (message->size() > 0)
				BIO_write(mInBio, message->data(), int(message->size())); // Input
			else
				recv(message); // Pass zero-sized messages through
		}

		std::lock_guard lock(mSslMutex);
		SSL_shutdown(mSsl);

	} catch (const std::exception &e) {
		PLOG_ERROR << "TLS recv: " << e.what();
	}

	if (state() == State::Connected) {
		PLOG_INFO << "TLS closed";
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		PLOG_ERROR << "TLS handshake failed";
		changeState(State::Failed);
	}
}

bool TlsTransport::flushOutput() {
	// Requires mSslMutex to be locked
	bool result = true;
	const size_t bufferSize = 4096;
	byte buffer[bufferSize];
	int len;
	while ((len = BIO_read(mOutBio, buffer, bufferSize)) > 0)
		result = outgoing(make_message(buffer, buffer + len));

	return result;
}

void TlsTransport::InfoCallback(const SSL *ssl, int where, int ret) {
	TlsTransport *t =
	    static_cast<TlsTransport *>(SSL_get_ex_data(ssl, TlsTransport::TransportExIndex));

	if (where & SSL_CB_ALERT) {
		if (ret != 256) { // Close Notify
			PLOG_ERROR << "TLS alert: " << SSL_alert_desc_string_long(ret);
		}
		t->mIncomingQueue.stop(); // Close the connection
	}
}

#endif

} // namespace rtc::impl

#endif
