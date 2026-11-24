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
		return outgoing(std::move(message)); // pass through

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
	mIncomingQueue.push(std::move(message));
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

				t->recv(std::move(message)); // Pass zero-sized messages through
				message.reset();
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

	mbedtls_ssl_init(&mSsl);
	mbedtls_ssl_config_init(&mConf);

	try {
		mbedtls::check(mbedtls_ssl_config_defaults(
		    &mConf, mIsClient ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
		    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));

		mbedtls_ssl_conf_max_tls_version(&mConf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_authmode(&mConf, MBEDTLS_SSL_VERIFY_NONE);
#if MBEDTLS_VERSION_MAJOR < 4
		mbedtls_ssl_conf_rng(&mConf, &mbedtls::random_func, nullptr);
#endif

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
		mbedtls_ssl_free(&mSsl);
		mbedtls_ssl_config_free(&mConf);
		throw;
	}
}

TlsTransport::~TlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying TLS transport";
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
		return outgoing(std::move(message)); // pass through

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
	mIncomingQueue.push(std::move(message));
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

				t->recv(std::move(message)); // Pass zero-sized messages through
				message.reset();
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

#elif USE_SCHANNEL

namespace {

const unsigned long ClientRequirements =
    ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
    ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS |
    ISC_REQ_MANUAL_CRED_VALIDATION;

const unsigned long ServerRequirements = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                                         ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                                         ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;

void acquire_credentials(CredHandle *handle, bool isClient, PCCERT_CONTEXT cert) {
	// The server certificate is verified manually by VerifiedTlsTransport
	const DWORD flags =
	    (isClient ? SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS : 0) |
	    SCH_USE_STRONG_CRYPTO;
	const unsigned long usage = isClient ? SECPKG_CRED_OUTBOUND : SECPKG_CRED_INBOUND;
	TimeStamp expiry;

	// SCH_CREDENTIALS is required for TLS 1.3 and available on Windows 10 version 1809 or later.
	// Enabled protocol versions are the system defaults.
	SCH_CREDENTIALS cred = {};
	cred.dwVersion = SCH_CREDENTIALS_VERSION;
	cred.dwFlags = flags;
	if (cert) {
		cred.cCreds = 1;
		cred.paCred = &cert;
	}

	SECURITY_STATUS status =
	    AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), usage, nullptr, &cred,
	                              nullptr, nullptr, handle, &expiry);
	if (status == SEC_E_OK)
		return;

	PLOG_DEBUG << "SCH_CREDENTIALS is unsupported (" << schannel::error_string(status)
	           << "), falling back to SCHANNEL_CRED";

	SCHANNEL_CRED legacyCred = {};
	legacyCred.dwVersion = SCHANNEL_CRED_VERSION;
	legacyCred.dwFlags = flags;
	legacyCred.grbitEnabledProtocols = 0; // system defaults
	if (cert) {
		legacyCred.cCreds = 1;
		legacyCred.paCred = &cert;
	}

	schannel::check(AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), usage,
	                                          nullptr, &legacyCred, nullptr, nullptr, handle,
	                                          &expiry),
	                "Failed to acquire TLS credentials");
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
      mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func), mCertificate(std::move(certificate)) {

	PLOG_DEBUG << "Initializing TLS transport (SChannel)";

	if (mIsClient && mHost)
		PLOG_VERBOSE << "Server Name Indication: " << *mHost;

	acquire_credentials(mCredentials.get(), mIsClient,
	                    mCertificate ? mCertificate->credentials() : nullptr);
}

TlsTransport::~TlsTransport() {
	stop();

	PLOG_DEBUG << "Destroying TLS transport";
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
		return outgoing(std::move(message)); // pass through

	PLOG_VERBOSE << "Send size=" << message->size();

	std::lock_guard lock(mSslMutex);
	const size_t headerSize = mStreamSizes.cbHeader;
	const size_t trailerSize = mStreamSizes.cbTrailer;
	const size_t maxSize = std::max(size_t(mStreamSizes.cbMaximumMessage), size_t(1));

	binary output;
	size_t offset = 0;
	while (offset < message->size()) {
		const size_t len = std::min(maxSize, message->size() - offset);
		const size_t pos = output.size();
		output.resize(pos + headerSize + len + trailerSize);
		std::memcpy(output.data() + pos + headerSize, message->data() + offset, len);

		SecBuffer buffers[4] = {};
		buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
		buffers[0].pvBuffer = output.data() + pos;
		buffers[0].cbBuffer = ULONG(headerSize);
		buffers[1].BufferType = SECBUFFER_DATA;
		buffers[1].pvBuffer = output.data() + pos + headerSize;
		buffers[1].cbBuffer = ULONG(len);
		buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
		buffers[2].pvBuffer = output.data() + pos + headerSize + len;
		buffers[2].cbBuffer = ULONG(trailerSize);
		buffers[3].BufferType = SECBUFFER_EMPTY;
		SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};

		schannel::check(EncryptMessage(mContext.get(), 0, &desc, 0), "TLS send failed");

		// Header and data stay contiguous, but the trailer size may change
		output.resize(pos + buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
		offset += len;
	}

	return outgoing(make_message(output.begin(), output.end()));
}

void TlsTransport::incoming(message_ptr message) {
	if (!message) {
		mIncomingQueue.stop();
		enqueueRecv();
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();
	mIncomingQueue.push(std::move(message));
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
		if (state() == State::Connecting && mIsClient) {
			std::lock_guard lock(mSslMutex);
			if (!mContext.valid())
				handshakeStep(); // Initiate the handshake
		}

		// Read incoming messages
		while (mIncomingQueue.running()) {
			auto next = mIncomingQueue.pop();
			if (!next)
				return;

			message_ptr message = std::move(*next);
			if (message->size() == 0) {
				recv(std::move(message)); // Pass zero-sized messages through
				continue;
			}

			bool finished = false;
			{
				std::lock_guard lock(mSslMutex);
				mInputBuffer.insert(mInputBuffer.end(), message->begin(), message->end());
				if (state() == State::Connecting)
					finished = handshakeStep();
			}

			if (finished) {
				PLOG_INFO << "TLS handshake finished";
				changeState(State::Connected);
				postHandshake();
			}

			if (state() == State::Connected) {
				std::vector<message_ptr> received;
				bool closed;
				{
					std::lock_guard lock(mSslMutex);
					closed = decryptInput(received);
				}

				for (auto &m : received)
					recv(std::move(m));

				if (closed) {
					PLOG_DEBUG << "TLS connection cleanly closed";
					break; // No more data can be read
				}
			}
		}

		if (state() == State::Connected)
			shutdown();

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

bool TlsTransport::handshakeStep() {
	// Requires mSslMutex to be locked
	std::wstring targetName = mIsClient && mHost ? schannel::to_wstring(*mHost) : std::wstring();

	while (true) {
		SecBuffer in[2] = {};
		in[0].BufferType = SECBUFFER_TOKEN;
		in[0].pvBuffer = mInputBuffer.empty() ? nullptr : mInputBuffer.data();
		in[0].cbBuffer = ULONG(mInputBuffer.size());
		in[1].BufferType = SECBUFFER_EMPTY; // receives SECBUFFER_EXTRA if any
		SecBufferDesc inDesc = {SECBUFFER_VERSION, 2, in};

		schannel::ContextBuffers out({SECBUFFER_TOKEN, SECBUFFER_ALERT, SECBUFFER_EMPTY});

		const bool hadContext = mContext.valid();
		ULONG attributes = 0;
		SECURITY_STATUS status;
		if (mIsClient)
			status =
			    InitializeSecurityContextW(mCredentials.get(), mContext.getOrNull(),
			                               targetName.empty() ? nullptr : targetName.data(),
			                               ClientRequirements, 0, 0, hadContext ? &inDesc : nullptr,
			                               0, mContext.get(), out.desc(), &attributes, nullptr);
		else
			status = AcceptSecurityContext(mCredentials.get(), mContext.getOrNull(), &inDesc,
			                               ServerRequirements, 0, mContext.get(), out.desc(),
			                               &attributes, nullptr);

		if (FAILED(status) && !hadContext)
			SecInvalidateHandle(mContext.get());

		// With ISC_REQ_EXTENDED_ERROR, the token contains an alert on failure
		if (const SecBuffer *token = out.find(SECBUFFER_TOKEN)) {
			auto data = static_cast<const byte *>(token->pvBuffer);
			outgoing(make_message(data, data + token->cbBuffer));
		}

		switch (status) {
		case SEC_E_INCOMPLETE_MESSAGE:
			return false; // Wait for more data

		case SEC_I_CONTINUE_NEEDED:
		case SEC_E_OK: {
			// Keep unprocessed data
			if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0 &&
			    in[1].cbBuffer <= mInputBuffer.size())
				mInputBuffer.erase(mInputBuffer.begin(), mInputBuffer.end() - in[1].cbBuffer);
			else
				mInputBuffer.clear();

			if (status == SEC_E_OK) {
				if (!mRenegotiating) {
					if (mIsClient && mVerifyServerCertificate) {
						try {
							verifyServerCertificate();
						} catch (const std::exception &) {
							// Notify the server before failing
							SCHANNEL_ALERT_TOKEN alert = {SCHANNEL_ALERT, TLS1_ALERT_FATAL,
							                              TLS1_ALERT_BAD_CERTIFICATE};
							try {
								sendControlToken(&alert, sizeof(alert));
							} catch (const std::exception &e) {
								PLOG_DEBUG << "Failed to send TLS alert: " << e.what();
							}
							throw;
						}
					}

					schannel::check(QueryContextAttributesW(
					                    mContext.get(), SECPKG_ATTR_STREAM_SIZES, &mStreamSizes),
					                "Failed to query TLS sizes");
				}
				return true;
			}

			if (mInputBuffer.empty())
				return false;

			continue;
		}

		default:
			throw std::runtime_error("Handshake failed: " + schannel::error_string(status));
		}
	}
}

bool TlsTransport::decryptInput(std::vector<message_ptr> &received) {
	// Requires mSslMutex to be locked
	while (!mInputBuffer.empty()) {
		if (mRenegotiating) {
			if (!handshakeStep())
				return false; // Wait for more data

			mRenegotiating = false;
			continue;
		}

		SecBuffer buffers[4] = {};
		buffers[0].BufferType = SECBUFFER_DATA;
		buffers[0].pvBuffer = mInputBuffer.data();
		buffers[0].cbBuffer = ULONG(mInputBuffer.size());
		buffers[1].BufferType = SECBUFFER_EMPTY;
		buffers[2].BufferType = SECBUFFER_EMPTY;
		buffers[3].BufferType = SECBUFFER_EMPTY;
		SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};

		SECURITY_STATUS status = DecryptMessage(mContext.get(), &desc, 0, nullptr);
		if (status == SEC_E_INCOMPLETE_MESSAGE)
			return false; // Wait for more data

		if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE && status != SEC_I_CONTEXT_EXPIRED)
			throw std::runtime_error("TLS decryption failed: " + schannel::error_string(status));

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

		mInputBuffer = std::move(extra);

		if (status == SEC_I_CONTEXT_EXPIRED) {
			mInputBuffer.clear();
			return true; // close_notify received
		}

		if (status == SEC_I_RENEGOTIATE) {
			// With TLS 1.3, this is a post-handshake message (like NewSessionTicket or KeyUpdate),
			// which must be processed by the handshake function. Otherwise, this is a
			// renegotiation request, which is refused like with OpenSSL, as the server certificate
			// would not be verified again.
			SecPkgContext_ConnectionInfo info = {};
			schannel::check(
			    QueryContextAttributesW(mContext.get(), SECPKG_ATTR_CONNECTION_INFO, &info),
			    "Failed to query TLS connection info");
			if (!(info.dwProtocol & (SP_PROT_TLS1_3_CLIENT | SP_PROT_TLS1_3_SERVER)))
				throw std::runtime_error("TLS renegotiation is not supported");

			PLOG_DEBUG << "TLS post-handshake message received";
			mRenegotiating = true;
			if (!handshakeStep())
				return false; // Wait for more data

			mRenegotiating = false;
		}
	}

	return false;
}

void TlsTransport::verifyServerCertificate() {
	// Requires mSslMutex to be locked
	PCCERT_CONTEXT remote = nullptr;
	schannel::check(
	    QueryContextAttributesW(mContext.get(), SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote),
	    "Failed to get server certificate");
	auto cert = schannel::make_cert_context_ptr(remote);
	if (!cert)
		throw std::runtime_error("Server certificate is missing");

	std::wstring serverName = schannel::to_wstring(mHost.value_or(""));

	auto verify = [&](HCERTCHAINENGINE engine) -> DWORD {
		LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH),
		                  const_cast<LPSTR>(szOID_SERVER_GATED_CRYPTO),
		                  const_cast<LPSTR>(szOID_SGC_NETSCAPE)};
		CERT_CHAIN_PARA chainPara = {};
		chainPara.cbSize = sizeof(chainPara);
		chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
		chainPara.RequestedUsage.Usage.cUsageIdentifier = DWORD(std::size(usages));
		chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = usages;

		// The remote certificate store contains the intermediate certificates sent by the server
		PCCERT_CHAIN_CONTEXT chain = nullptr;
		if (!CertGetCertificateChain(engine, cert.get(), nullptr, cert->hCertStore, &chainPara, 0,
		                             nullptr, &chain))
			return GetLastError();

		HTTPSPolicyCallbackData sslPolicy = {};
		sslPolicy.cbStruct = sizeof(sslPolicy);
		sslPolicy.dwAuthType = AUTHTYPE_SERVER;
		sslPolicy.pwszServerName = serverName.data();

		CERT_CHAIN_POLICY_PARA policyPara = {};
		policyPara.cbSize = sizeof(policyPara);
		policyPara.pvExtraPolicyPara = &sslPolicy;

		CERT_CHAIN_POLICY_STATUS policyStatus = {};
		policyStatus.cbSize = sizeof(policyStatus);

		BOOL success = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policyPara,
		                                                &policyStatus);
		DWORD error = success ? policyStatus.dwError : GetLastError();
		CertFreeCertificateChain(chain);
		return error;
	};

	string caError;
	if (mCaCertificates) {
		// Trust the specified CA certificates
		CERT_CHAIN_ENGINE_CONFIG config = {};
		config.cbSize = sizeof(config);
		config.hExclusiveRoot = mCaCertificates.get();
		HCERTCHAINENGINE engine = nullptr;
		if (CertCreateCertificateChainEngine(&config, &engine)) {
			DWORD error = verify(engine);
			CertFreeCertificateChainEngine(engine);
			if (error == 0) {
				PLOG_DEBUG << "Server certificate verified with the specified CA certificate";
				return;
			}

			// Only fall back to the system root certificates if the chain does not lead to the
			// specified CA certificates, otherwise the actual error, like a name mismatch, would
			// be hidden by an untrusted root error.
			if (long(error) != CERT_E_UNTRUSTEDROOT && long(error) != CERT_E_CHAINING)
				throw std::runtime_error("TLS certificate verification failed: " +
				                         schannel::error_string(long(error)));

			caError = schannel::error_string(long(error));
			PLOG_DEBUG
			    << "Server certificate verification with the specified CA certificate failed: "
			    << caError;
		} else {
			PLOG_WARNING << "Failed to create certificate chain engine: "
			             << schannel::error_string(long(GetLastError()));
		}
	}

	// Trust the system root certificates
	DWORD error = verify(nullptr);
	if (error != 0)
		throw std::runtime_error(
		    "TLS certificate verification failed: " + schannel::error_string(long(error)) +
		    (caError.empty() ? "" : " (with the specified CA certificate: " + caError + ")"));

	PLOG_DEBUG << "Server certificate verified";
}

void TlsTransport::sendControlToken(void *token, unsigned long size) {
	// Requires mSslMutex to be locked
	SecBuffer control = {size, SECBUFFER_TOKEN, token};
	SecBufferDesc controlDesc = {SECBUFFER_VERSION, 1, &control};
	schannel::check(ApplyControlToken(mContext.get(), &controlDesc), "ApplyControlToken failed");

	// The handshake function generates the corresponding message
	SecBuffer in[2] = {};
	in[0].BufferType = SECBUFFER_TOKEN;
	in[1].BufferType = SECBUFFER_EMPTY;
	SecBufferDesc inDesc = {SECBUFFER_VERSION, 2, in};

	schannel::ContextBuffers out({SECBUFFER_TOKEN, SECBUFFER_ALERT, SECBUFFER_EMPTY});
	ULONG attributes = 0;
	SECURITY_STATUS status;
	if (mIsClient)
		status = InitializeSecurityContextW(mCredentials.get(), mContext.get(), nullptr,
		                                    ClientRequirements, 0, 0, nullptr, 0, mContext.get(),
		                                    out.desc(), &attributes, nullptr);
	else
		status =
		    AcceptSecurityContext(mCredentials.get(), mContext.get(), &inDesc, ServerRequirements,
		                          0, mContext.get(), out.desc(), &attributes, nullptr);

	schannel::check(status, "Failed to generate the TLS message");
	if (const SecBuffer *token = out.find(SECBUFFER_TOKEN)) {
		auto data = static_cast<const byte *>(token->pvBuffer);
		outgoing(make_message(data, data + token->cbBuffer));
	}
}

void TlsTransport::shutdown() {
	std::lock_guard lock(mSslMutex);
	if (!mContext.valid())
		return;

	try {
		// Send the close_notify alert
		DWORD type = SCHANNEL_SHUTDOWN;
		sendControlToken(&type, sizeof(type));

	} catch (const std::exception &e) {
		PLOG_DEBUG << "TLS shutdown failed: " << e.what();
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
#if OPENSSL_VERSION_NUMBER >= 0x40000000
			openssl::check(SSL_set1_dnsname(mSsl, mHost->c_str()), "Failed to set SSL dnsname");
#else
			openssl::check(SSL_set1_host(mSsl, mHost->c_str()), "Failed to set SSL host");
#endif
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

	openssl::check_error(err, "Handshake failed", mSsl);
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
		return outgoing(std::move(message)); // pass through

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
	mIncomingQueue.push(std::move(message));
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
			if (message->size() == 0) {
				recv(std::move(message)); // Pass zero-sized messages through
				continue;
			}

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
					flushOutput();
				}

				if (openssl::check_error(err, "Handshake failed", mSsl)) {
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
						if (!incomingWritten) {
							BIO_write(mInBio, message->data(), int(message->size()));
							incomingWritten = true;
						}
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
