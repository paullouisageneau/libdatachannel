/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_TLS_TRANSPORT_H
#define RTC_IMPL_TLS_TRANSPORT_H

#include "certificate.hpp"
#include "common.hpp"
#include "queue.hpp"
#include "tls.hpp"
#include "transport.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <atomic>
#include <thread>

namespace rtc::impl {

class TcpTransport;

class TlsTransport : public Transport {
public:
	static void Init();
	static void Cleanup();

	TlsTransport(shared_ptr<TcpTransport> lower, optional<string> host, certificate_ptr certificate,
	             state_callback callback);
	virtual ~TlsTransport();

	void start() override;
	void stop() override;
	bool send(message_ptr message) override;

	bool isClient() const { return mIsClient; }

protected:
	virtual void incoming(message_ptr message) override;
	virtual bool outgoing(message_ptr message) override;
	virtual void postHandshake();

	void runRecvLoop();

	const optional<string> mHost;
	const bool mIsClient;

	Queue<message_ptr> mIncomingQueue;
	std::thread mRecvThread;
	std::atomic<bool> mStarted = false;

#if USE_GNUTLS
	gnutls_session_t mSession;

	message_ptr mIncomingMessage;
	size_t mIncomingMessagePosition = 0;
	std::atomic<bool> mOutgoingResult = true;

	static ssize_t WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len);
	static ssize_t ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen);
	static int TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms);
#else
	SSL_CTX *mCtx;
	SSL *mSsl;
	BIO *mInBio, *mOutBio;
	std::mutex mSslMutex;

	bool flushOutput();

	static int TransportExIndex;
	static void InfoCallback(const SSL *ssl, int where, int ret);

#endif
};

} // namespace rtc::impl

#endif

#endif
