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

#include <gnutls/dtls.h>

using std::shared_ptr;
using std::string;

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

using std::shared_ptr;

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
	const char *priorities = "SECURE128:-VERS-SSL3.0:-VERS-TLS1.0:-ARCFOUR-128:-COMP-ALL";
	const char *err_pos = NULL;
	check_gnutls(gnutls_priority_set_direct(mSession, priorities, &err_pos),
	             "Unable to set TLS priorities");

	check_gnutls(
	    gnutls_credentials_set(mSession, GNUTLS_CRD_CERTIFICATE, mCertificate->credentials()));

	gnutls_dtls_set_mtu(mSession, 1280 - 40 - 8); // min MTU over UDP/IPv6
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
	onRecv(nullptr); // unset recv callback

	mIncomingQueue.stop();
	mRecvThread.join();

	gnutls_bye(mSession, GNUTLS_SHUT_RDWR);
	gnutls_deinit(mSession);
}

DtlsTransport::State DtlsTransport::state() const { return mState; }

bool DtlsTransport::send(message_ptr message) {
	if (!message)
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
	const size_t maxMtu = 2048;

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
