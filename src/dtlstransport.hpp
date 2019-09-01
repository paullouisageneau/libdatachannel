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

#include <functional>
#include <memory>
#include <thread>

#include <gnutls/gnutls.h>

namespace rtc {

class IceTransport;

class DtlsTransport : public Transport {
public:
	using verifier_callback = std::function<bool(const std::string &fingerprint)>;
	using ready_callback = std::function<void(void)>;

	DtlsTransport(std::shared_ptr<IceTransport> lower, std::shared_ptr<Certificate> certificate,
	              verifier_callback verifier, ready_callback ready);
	~DtlsTransport();

	bool send(message_ptr message);

private:
	void incoming(message_ptr message);
	void runRecvLoop();

	const std::shared_ptr<Certificate> mCertificate;

	gnutls_session_t mSession;
	Queue<message_ptr> mIncomingQueue;
	std::thread mRecvThread;
	verifier_callback mVerifierCallback;
	ready_callback mReadyCallback;

	static int CertificateCallback(gnutls_session_t session);
	static ssize_t WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len);
	static ssize_t ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen);
	static int TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms);
};

} // namespace rtc

#endif

