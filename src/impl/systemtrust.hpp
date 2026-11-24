/**
 * Copyright (c) 2026 Nemirtingas
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_SYSTEM_TRUST_H
#define RTC_IMPL_SYSTEM_TRUST_H

#include "common.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <vector>

// Windows and Apple platforms expose an API to verify a certificate chain against the system trust
// store. Other platforms only provide CA bundle files that the TLS backend must load by itself.
#if defined(_WIN32) || defined(__APPLE__)
#define RTC_SYSTEM_TRUST_STORE 1
#else
#define RTC_SYSTEM_TRUST_STORE 0
#endif

namespace rtc::impl::system_trust {

enum class Status {
	Trusted,
	UntrustedRoot,
	HostnameMismatch,
	Expired,
	NotYetValid,
	Revoked,
	Other,
};

struct Result {
	Status status = Status::Other;
	string message;

	bool trusted() const { return status == Status::Trusted; }
};

string to_string(Status status);

#if RTC_SYSTEM_TRUST_STORE

// Verify the certificate chain presented by a TLS server against the system trust store, including
// the server name check. The chain contains DER-encoded certificates, the server certificate first,
// followed by the intermediate certificates sent by the server. The host is a DNS name or an IP
// address literal (without brackets or zone identifier).
// Note this call may block, as the system may fetch missing intermediate certificates or
// revocation information over the network.
Result verify_server_chain(const std::vector<binary> &chain, const string &host);

#else

// Candidate CA bundle files, in order of preference
std::vector<string> ca_bundle_files();

// Candidate CA certificate directories, used if no bundle file can be loaded
std::vector<string> ca_bundle_directories();

#endif

} // namespace rtc::impl::system_trust

#endif

#endif
