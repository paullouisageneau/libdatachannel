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

#include "tlstransport.hpp"
#include "tcptransport.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>

using namespace std::chrono;

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::weak_ptr;

#if USE_GNUTLS

namespace {

static bool check_gnutls(int ret, const string &message = "GnuTLS error") {
	if (ret < 0) {
		if (!gnutls_error_is_fatal(ret)) {
			PLOG_INFO << gnutls_strerror(ret);
			return false;
		}
		PLOG_ERROR << message << ": " << gnutls_strerror(ret);
		throw std::runtime_error(message + ": " + gnutls_strerror(ret));
	}
	return true;
}

} // namespace

namespace rtc {

TlsTransport::TlsTransport(shared_ptr<TcpTransport> lower, const string &host)
    : Transport(lower), mHost(host) {

	PLOG_DEBUG << "Initializing TLS transport (GnuTLS)";

	check_gnutls(gnutls_init(&mSession, GNUTLS_CLIENT));

	try {
		const char *priorities = "SECURE128:-VERS-SSL3.0:-ARCFOUR-128";
		const char *err_pos = NULL;
		check_gnutls(gnutls_priority_set_direct(mSession, priorities, &err_pos),
		             "Unable to set TLS priorities");

		gnutls_session_set_ptr(mSession, this);
		gnutls_transport_set_ptr(mSession, this);
		gnutls_transport_set_push_function(mSession, WriteCallback);
		gnutls_transport_set_pull_function(mSession, ReadCallback);
		gnutls_transport_set_pull_timeout_function(mSession, TimeoutCallback);

		gnutls_server_name_set(mSession, GNUTLS_NAME_DNS, host.data(), host.size());

		mRecvThread = std::thread(&DtlsTransport::runRecvLoop, this);

	} catch (...) {

		gnutls_deinit(mSession);
		throw;
	}
}

TlsTransport::~DtlsTransport() {
	stop();
	gnutls_deinit(mSession);
}

bool DtlsTransport::stop() {
	if (!Transport::stop())
		return false;

	PLOG_DEBUG << "Stopping TLS recv thread";
	mIncomingQueue.stop();
	mRecvThread.join();
	return true;
}

bool DtlsTransport::send(message_ptr message) {
	if (!message)
		return false;

	ssize_t ret;
	do {
		ret = gnutls_record_send(mSession, message->data(), message->size());
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	return check_gnutls(ret);
}

void DtlsTransport::incoming(message_ptr message) {
	if (message)
		mIncomingQueue.push(message);
	else
		mIncomingQueue.stop();
}

void TlsTransport::runRecvLoop() {
	const size_t bufferSize = 4096;

	// Handshake loop
	try {
		int ret;
		do {
			ret = gnutls_handshake(mSession);
		} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN ||
		         !check_gnutls(ret, "TLS handshake failed"));

	} catch (const std::exception &e) {
		PLOG_ERROR << "TLS handshake: " << e.what();
		changeState(State::Failed);
		return;
	}

	// Receive loop
	try {
		while (true) {
			char buffer[bufferSize];
			ssize_t ret;
			do {
				ret = gnutls_record_recv(mSession, buffer, bufferSize);
			} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

			// Consider premature termination as remote closing
			if (ret == GNUTLS_E_PREMATURE_TERMINATION) {
				PLOG_DEBUG << "TLS connection terminated";
				break;
			}

			if (check_gnutls(ret)) {
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

	gnutls_bye(mSession, GNUTLS_SHUT_RDWR);

	PLOG_INFO << "TLS disconnected";
	recv(nullptr);
}

ssize_t TlsTransport::WriteCallback(gnutls_transport_ptr_t ptr, const void *data, size_t len) {
	DtlsTransport *t = static_cast<DtlsTransport *>(ptr);
	if (len > 0) {
		auto b = reinterpret_cast<const byte *>(data);
		t->outgoing(make_message(b, b + len));
	}
	gnutls_transport_set_errno(t->mSession, 0);
	return ssize_t(len);
}

ssize_t TlsTransport::ReadCallback(gnutls_transport_ptr_t ptr, void *data, size_t maxlen) {
	TlsTransport *t = static_cast<DtlsTransport *>(ptr);
	if (auto next = t->mIncomingQueue.pop()) {
		auto message = *next;
		ssize_t len = std::min(maxlen, message->size());
		std::memcpy(data, message->data(), len);
		gnutls_transport_set_errno(t->mSession, 0);
		return len;
	}
	// Closed
	gnutls_transport_set_errno(t->mSession, 0);
	return 0;
}

int TlsTransport::TimeoutCallback(gnutls_transport_ptr_t ptr, unsigned int ms) {
	TlsTransport *t = static_cast<DtlsTransport *>(ptr);
	if (ms != GNUTLS_INDEFINITE_TIMEOUT)
		t->mIncomingQueue.wait(milliseconds(ms));
	else
		t->mIncomingQueue.wait();
	return !t->mIncomingQueue.empty() ? 1 : 0;
}

} // namespace rtc

#else // USE_GNUTLS==0
// TODO
#endif

