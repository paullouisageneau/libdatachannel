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

#ifndef RTC_DTLS_TRANSPORT_H
#define RTC_DTLS_TRANSPORT_H

#include "certificate.hpp"
#include "include.hpp"
#include "peerconnection.hpp"
#include "queue.hpp"
#include "transport.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#if USE_GNUTLS
#include <gnutls/gnutls.h>
#else
#include <openssl/ssl.h>
#endif

namespace rtc {

class IceTransport;

class DtlsTransport : public Transport {
public:
	enum class State { Disconnected, Connecting, Connected, Failed };

	using verifier_callback = std::function<bool(const std::string &fingerprint)>;
	using state_callback = std::function<void(State state)>;

	DtlsTransport(std::shared_ptr<IceTransport> lower, std::shared_ptr<Certificate> certificate,
	              verifier_callback verifierCallback, state_callback stateChangeCallback);
	~DtlsTransport();

	State state() const;

	void stop() override;
	bool send(message_ptr message); // false if dropped

private:
	void incoming(message_ptr message);
	void changeState(State state);
	void runRecvLoop();

	const std::shared_ptr<Certificate> mCertificate;

	Queue<message_ptr> mIncomingQueue;
	std::atomic<State> mState;
	std::thread mRecvThread;

	verifier_callback mVerifierCallback;
	state_callback mStateChangeCallback;

#if USE_GNUTLS
	gnutls_session_t mSession;

	static int CertificateCallback(gnutls_session_t session);
	static ssize_t WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len);
	static ssize_t ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen);
	static int TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms);
#else
	SSL_CTX *mCtx;
	SSL *mSsl;
	BIO *mInBio, *mOutBio;

	static int TransportExIndex;
	static std::mutex GlobalMutex;

	static void GlobalInit();
	static int CertificateCallback(int preverify_ok, X509_STORE_CTX *ctx);
	static void InfoCallback(const SSL *ssl, int where, int ret);
#endif
};

} // namespace rtc

#endif

