/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tlstransport.hpp"
#include "httpproxytransport.hpp"
#include "tcptransport.hpp"
#include "threadpool.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>

using namespace std::chrono;

namespace rtc::impl {

void TlsTransport::enqueueRecv() {
	if (mPendingRecvCount > 0)
		return;

	if (auto shared_this = weak_from_this().lock()) {
		++mPendingRecvCount;
		ThreadPool::Instance().enqueue(&TlsTransport::doRecv, std::move(shared_this));
	}
}

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

TlsTransport::TlsTransport(variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>> lower,
                           optional<string> host, certificate_ptr certificate,
                           state_callback callback)
    : Transport(std::visit([](auto l) { return std::static_pointer_cast<Transport>(l); }, lower),
                std::move(callback)),
      mHost(std::move(host)), mIsClient(std::visit([](auto l) { return l->isActive(); }, lower)),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing TLS transport (GnuTLS)";

	unsigned int flags = GNUTLS_NONBLOCK | (mIsClient ? GNUTLS_CLIENT : GNUTLS_SERVER);
	gnutls::check(gnutls_init(&mSession, flags));

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

	PLOG_DEBUG << "Destroying TLS transport";
	gnutls_deinit(mSession);
}

void TlsTransport::start() {
	PLOG_DEBUG << "Starting TLS transport";
	registerIncoming();
	changeState(State::Connecting);
	enqueueRecv(); // to initiate the handshake
}

void TlsTransport::stop() {
	PLOG_DEBUG << "Stopping TLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
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
		enqueueRecv();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
	enqueueRecv();
}

bool TlsTransport::outgoing(message_ptr message) {
	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

void TlsTransport::postHandshake() {
	// Dummy
}

void TlsTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;

	const size_t bufferSize = 4096;
	char buffer[bufferSize];

	try {
		// Handle handshake if connecting
		if (state() == State::Connecting) {
			int ret;
			do {
				ret = gnutls_handshake(mSession);

				if (ret == GNUTLS_E_AGAIN)
					return;

			} while (!gnutls::check(ret, "Handshake failed")); // Re-call on non-fatal error

			PLOG_INFO << "TLS handshake finished";
			changeState(State::Connected);
			postHandshake();
		}

		if (state() == State::Connected) {
			while (true) {
				ssize_t ret = gnutls_record_recv(mSession, buffer, bufferSize);

				if (ret == GNUTLS_E_AGAIN)
					return;

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
		}
	} catch (const std::exception &e) {
		PLOG_ERROR << "TLS recv: " << e.what();
	}

	gnutls_bye(mSession, GNUTLS_SHUT_WR);

	if (state() == State::Connected) {
		PLOG_INFO << "TLS closed";
		changeState(State::Disconnected);
		recv(nullptr);
	} else {
		PLOG_ERROR << "TLS handshake failed";
		changeState(State::Failed);
	}
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
		} else if (t->mIncomingQueue.running()) {
			gnutls_transport_set_errno(t->mSession, EAGAIN);
			return -1;
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

int TlsTransport::TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int /* ms */) {
	TlsTransport *t = static_cast<TlsTransport *>(ptr);
	try {
		message_ptr &message = t->mIncomingMessage;
		size_t &position = t->mIncomingMessagePosition;

		if (message && position < message->size())
			return 1;

		return !t->mIncomingQueue.empty() ? 1 : 0;

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return 1;
	}
}

#elif USE_MBEDTLS

void TlsTransport::Init() {
	// Nothing to do
}

void TlsTransport::Cleanup() {
	// Nothing to do
}

TlsTransport::TlsTransport(variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>> lower,
                           optional<string> host, certificate_ptr certificate,
                           state_callback callback)
    : Transport(std::visit([](auto l) { return std::static_pointer_cast<Transport>(l); }, lower),
                std::move(callback)),
      mHost(std::move(host)), mIsClient(std::visit([](auto l) { return l->isActive(); }, lower)),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing TLS transport (MbedTLS)";

	psa_crypto_init();
	mbedtls_entropy_init(&mEntropy);
	mbedtls_ctr_drbg_init(&mDrbg);
	mbedtls_ssl_init(&mSsl);
	mbedtls_ssl_config_init(&mConf);
	mbedtls_ctr_drbg_set_prediction_resistance(&mDrbg, MBEDTLS_CTR_DRBG_PR_ON);

	try {
		mbedtls::check(mbedtls_ctr_drbg_seed(&mDrbg, mbedtls_entropy_func, &mEntropy, NULL, 0));

		mbedtls::check(mbedtls_ssl_config_defaults(
		    &mConf, mIsClient ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
		    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));

		mbedtls_ssl_conf_max_version(&mConf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3); // TLS 1.2
		mbedtls_ssl_conf_authmode(&mConf, MBEDTLS_SSL_VERIFY_OPTIONAL);
		mbedtls_ssl_conf_rng(&mConf, mbedtls_ctr_drbg_random, &mDrbg);

		if (certificate) {
			auto [crt, pk] = certificate->credentials();
			mbedtls::check(mbedtls_ssl_conf_own_cert(&mConf, crt.get(), pk.get()));
		}

		if (mIsClient && mHost) {
			PLOG_VERBOSE << "Server Name Indication: " << *mHost;
			mbedtls_ssl_set_hostname(&mSsl, mHost->c_str());
		}

		mbedtls::check(mbedtls_ssl_setup(&mSsl, &mConf));
		mbedtls_ssl_set_bio(&mSsl, static_cast<void *>(this), WriteCallback, ReadCallback, NULL);

	} catch (...) {
		mbedtls_entropy_free(&mEntropy);
		mbedtls_ctr_drbg_free(&mDrbg);
		mbedtls_ssl_free(&mSsl);
		mbedtls_ssl_config_free(&mConf);
		throw;
	}
}

TlsTransport::~TlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying TLS transport";
	mbedtls_entropy_free(&mEntropy);
	mbedtls_ctr_drbg_free(&mDrbg);
	mbedtls_ssl_free(&mSsl);
	mbedtls_ssl_config_free(&mConf);
}

void TlsTransport::start() {
	PLOG_DEBUG << "Starting TLS transport";
	registerIncoming();
	changeState(State::Connecting);
	enqueueRecv(); // to initiate the handshake
}

void TlsTransport::stop() {
	PLOG_DEBUG << "Stopping TLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
}

bool TlsTransport::send(message_ptr message) {
	if (state() != State::Connected)
		throw std::runtime_error("TLS is not open");

	if (!message || message->size() == 0)
		return outgoing(message); // pass through

	PLOG_VERBOSE << "Send size=" << message->size();

	int ret;
	do {
		std::lock_guard lock(mSslMutex);
		ret = mbedtls_ssl_write(&mSsl, reinterpret_cast<const unsigned char *>(message->data()),
		                        int(message->size()));
	} while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);

	if (!mbedtls::check(ret))
		throw std::runtime_error("TLS send failed");

	return mOutgoingResult;
}

void TlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		enqueueRecv();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
	enqueueRecv();
}

bool TlsTransport::outgoing(message_ptr message) {
	bool result = Transport::outgoing(std::move(message));
	mOutgoingResult = result;
	return result;
}

void TlsTransport::postHandshake() {
	// Dummy
}

void TlsTransport::doRecv() {
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
					return;
				}

				if (mbedtls::check(ret, "Handshake failed")) {
					PLOG_INFO << "TLS handshake finished";
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
					PLOG_DEBUG << "TLS connection cleanly closed";
					break;
				}

				if (mbedtls::check(ret)) {
					if (ret == 0) {
						PLOG_DEBUG << "TLS connection terminated";
						break;
					}
					auto *b = reinterpret_cast<byte *>(buffer);
					recv(make_message(b, b + ret));
				}
			}
		}
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

int TlsTransport::WriteCallback(void *ctx, const unsigned char *buf, size_t len) {
	auto *t = static_cast<TlsTransport *>(ctx);
	auto *b = reinterpret_cast<const byte *>(buf);
	t->outgoing(make_message(b, b + len));

	return int(len);
}

int TlsTransport::ReadCallback(void *ctx, unsigned char *buf, size_t len) {
	TlsTransport *t = static_cast<TlsTransport *>(ctx);
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
			size_t writeLen = std::min(len, available);
			std::memcpy(buf, message->data() + position, writeLen);
			position += writeLen;
			return int(writeLen);
		} else if (t->mIncomingQueue.running()) {
			return MBEDTLS_ERR_SSL_WANT_READ;
		} else {
			return MBEDTLS_ERR_SSL_CONN_EOF;
		}

	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
}

#else

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

TlsTransport::TlsTransport(variant<shared_ptr<TcpTransport>, shared_ptr<HttpProxyTransport>> lower,
                           optional<string> host, certificate_ptr certificate,
                           state_callback callback)
    : Transport(std::visit([](auto l) { return std::static_pointer_cast<Transport>(l); }, lower),
                std::move(callback)),
      mHost(std::move(host)), mIsClient(std::visit([](auto l) { return l->isActive(); }, lower)),
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func) {

	PLOG_DEBUG << "Initializing TLS transport (OpenSSL)";

	try {
		if (!(mCtx = SSL_CTX_new(TLS_method()))) // version-flexible
			throw std::runtime_error("Failed to create SSL context");

		openssl::check(SSL_CTX_set_cipher_list(mCtx, "ALL:!LOW:!EXP:!RC4:!MD5:@STRENGTH"),
		               "Failed to set SSL priorities");

#if OPENSSL_VERSION_NUMBER >= 0x30000000
		openssl::check(SSL_CTX_set1_groups_list(mCtx, "P-256"), "Failed to set SSL groups");
#else
		auto ecdh = unique_ptr<EC_KEY, decltype(&EC_KEY_free)>(
		    EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
		SSL_CTX_set_tmp_ecdh(mCtx, ecdh.get());
#endif

		if(mIsClient) {
			if (!SSL_CTX_set_default_verify_paths(mCtx)) {
				PLOG_WARNING << "SSL root CA certificates unavailable";
			}
		}

		if (certificate) {
			auto [x509, pkey] = certificate->credentials();
			SSL_CTX_use_certificate(mCtx, x509);
			SSL_CTX_use_PrivateKey(mCtx, pkey);

			for (auto c : certificate->chain())
				SSL_CTX_add1_chain_cert(mCtx, c); // add1 increments reference count
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

	PLOG_DEBUG << "Destroying TLS transport";
	SSL_free(mSsl);
	SSL_CTX_free(mCtx);
}

void TlsTransport::start() {
	PLOG_DEBUG << "Starting TLS transport";
	registerIncoming();
	changeState(State::Connecting);

	// Initiate the handshake
	int ret, err;
	{
		std::lock_guard lock(mSslMutex);
		ret = SSL_do_handshake(mSsl);
		err = SSL_get_error(mSsl, ret);
		flushOutput();
	}

	openssl::check_error(err, "Handshake failed");
}

void TlsTransport::stop() {
	PLOG_DEBUG << "Stopping TLS transport";
	unregisterIncoming();
	mIncomingQueue.stop();
	enqueueRecv();
}

bool TlsTransport::send(message_ptr message) {
	if (state() != State::Connected)
		throw std::runtime_error("TLS is not open");

	if (!message || message->size() == 0)
		return outgoing(message); // pass through

	PLOG_VERBOSE << "Send size=" << message->size();

	int err;
	bool result;
	{
		std::lock_guard lock(mSslMutex);
		int ret = SSL_write(mSsl, message->data(), int(message->size()));
		err = SSL_get_error(mSsl, ret);
		result = flushOutput();
	}

	if (!openssl::check_error(err))
		throw std::runtime_error("TLS send failed");

	return result;
}

void TlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		enqueueRecv();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(message);
	enqueueRecv();
}

bool TlsTransport::outgoing(message_ptr message) { return Transport::outgoing(std::move(message)); }

void TlsTransport::postHandshake() {
	// Dummy
}

void TlsTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;

	if (state() != State::Connecting && state() != State::Connected)
		return;

	try {
		const size_t bufferSize = 4096;
		byte buffer[bufferSize];

		// Read incoming messages
		while (mIncomingQueue.running()) {
			auto next = mIncomingQueue.pop();
			if (!next)
				return;

			message_ptr message = std::move(*next);
			if (message->size() > 0)
				BIO_write(mInBio, message->data(), int(message->size())); // Input
			else
				recv(message); // Pass zero-sized messages through

			if (state() == State::Connecting) {
				// Continue the handshake
				int ret, err;
				{
					std::lock_guard lock(mSslMutex);
					ret = SSL_do_handshake(mSsl);
					err = SSL_get_error(mSsl, ret);
					flushOutput();
				}

				if (openssl::check_error(err, "Handshake failed")) {
					PLOG_INFO << "TLS handshake finished";
					changeState(State::Connected);
					postHandshake();
				}
			}

			if (state() == State::Connected) {
				int ret, err;
				while (true) {
					{
						std::lock_guard lock(mSslMutex);
						ret = SSL_read(mSsl, buffer, bufferSize);
						err = SSL_get_error(mSsl, ret);
						flushOutput(); // SSL_read() can also cause write operations
					}

					if (err == SSL_ERROR_ZERO_RETURN)
						break;

					if (openssl::check_error(err))
						recv(make_message(buffer, buffer + ret));
					else
						break;
				}

				if (err == SSL_ERROR_ZERO_RETURN) {
					PLOG_DEBUG << "TLS connection cleanly closed";
					break; // No more data can be read
				}
			}
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
