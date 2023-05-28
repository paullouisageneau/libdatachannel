/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_DTLS_TRANSPORT_H
#define RTC_IMPL_DTLS_TRANSPORT_H

#include "certificate.hpp"
#include "common.hpp"
#include "queue.hpp"
#include "tls.hpp"
#include "transport.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace rtc::impl {

class IceTransport;

class DtlsTransport : public Transport {
public:
	static void Init();
	static void Cleanup();

	using verifier_callback = std::function<bool(const std::string &fingerprint)>;

	DtlsTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate, optional<size_t> mtu,
	              verifier_callback verifierCallback, state_callback stateChangeCallback);
	~DtlsTransport();

	virtual void start() override;
	virtual void stop() override;
	virtual bool send(message_ptr message) override; // false if dropped

	bool isClient() const { return mIsClient; }

protected:
	virtual void incoming(message_ptr message) override;
	virtual bool outgoing(message_ptr message) override;
	virtual bool demuxMessage(message_ptr message);
	virtual void postHandshake();

	void runRecvLoop();

	const optional<size_t> mMtu;
	const certificate_ptr mCertificate;
	const verifier_callback mVerifierCallback;
	const bool mIsClient;

	Queue<message_ptr> mIncomingQueue;
	std::thread mRecvThread;
	std::atomic<bool> mStarted = false;
	std::atomic<unsigned int> mCurrentDscp = 0;
	std::atomic<bool> mOutgoingResult = true;

#if USE_GNUTLS
	gnutls_session_t mSession;
	std::mutex mSendMutex;

	static int CertificateCallback(gnutls_session_t session);
	static ssize_t WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len);
	static ssize_t ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen);
	static int TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms);
#else
	SSL_CTX *mCtx = NULL;
	SSL *mSsl = NULL;
	BIO *mInBio, *mOutBio;
	std::mutex mSslMutex;

	static BIO_METHOD *BioMethods;
	static int TransportExIndex;
	static std::mutex GlobalMutex;

	static int CertificateCallback(int preverify_ok, X509_STORE_CTX *ctx);
	static void InfoCallback(const SSL *ssl, int where, int ret);

	static int BioMethodNew(BIO *bio);
	static int BioMethodFree(BIO *bio);
	static int BioMethodWrite(BIO *bio, const char *in, int inl);
	static long BioMethodCtrl(BIO *bio, int cmd, long num, void *ptr);
#endif
};

} // namespace rtc::impl

#endif
