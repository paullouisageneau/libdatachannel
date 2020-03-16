/**
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#ifndef RTC_TLS_TRANSPORT_H
#define RTC_TLS_TRANSPORT_H

#if ENABLE_WEBSOCKET

#include "include.hpp"
#include "queue.hpp"
#include "transport.hpp"

#include <memory>
#include <mutex>
#include <thread>

#if USE_GNUTLS
#include <gnutls/gnutls.h>
#else
#include <openssl/ssl.h>
#endif

namespace rtc {

class TcpTransport;

class TlsTransport : public Transport {
public:
	static void Init();
	static void Cleanup();

	TlsTransport(std::shared_ptr<TcpTransport> lower, string host, state_callback callback);
	~TlsTransport();

	bool stop() override;
	bool send(message_ptr message) override;

	void incoming(message_ptr message) override;

protected:
	void runRecvLoop();

	Queue<message_ptr> mIncomingQueue;
	std::thread mRecvThread;

#if USE_GNUTLS
	gnutls_session_t mSession;

	static ssize_t WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len);
	static ssize_t ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen);
	static int TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms);
#else
	SSL_CTX *mCtx;
	SSL *mSsl;
	BIO *mInBio, *mOutBio;

	static int TransportExIndex;

	static int CertificateCallback(int preverify_ok, X509_STORE_CTX *ctx);
	static void InfoCallback(const SSL *ssl, int where, int ret);
#endif
};

} // namespace rtc

#endif

#endif
