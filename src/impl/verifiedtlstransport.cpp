/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "verifiedtlstransport.hpp"
#include "common.hpp"

#if RTC_ENABLE_WEBSOCKET

namespace rtc::impl {

VerifiedTlsTransport::VerifiedTlsTransport(shared_ptr<TcpTransport> lower, string host,
                                           certificate_ptr certificate, state_callback callback)
    : TlsTransport(std::move(lower), std::move(host), std::move(certificate), std::move(callback)) {

#if USE_GNUTLS
	PLOG_DEBUG << "Setting up TLS certificate verification";
	gnutls_session_set_verify_cert(mSession, mHost->c_str(), 0);
#else
	PLOG_DEBUG << "Setting up TLS certificate verification";
	SSL_set_verify(mSsl, SSL_VERIFY_PEER, NULL);
	SSL_set_verify_depth(mSsl, 4);
#endif
}

VerifiedTlsTransport::~VerifiedTlsTransport() { stop(); }

} // namespace rtc::impl

#endif
