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

#include "dtlssrtptransport.hpp"

#include <exception>

using std::shared_ptr;

namespace rtc {

DtlsSrtpTransport::DtlsSrtpTransport(std::shared_ptr<IceTransport> lower,
                                     shared_ptr<Certificate> certificate,
                                     verifier_callback verifierCallback,
                                     message_callback recvCallback,
                                     state_callback stateChangeCallback)
    : DtlsTransport(lower, certificate, std::move(verifierCallback),
                    std::move(stateChangeCallback)) {
	onRecv(recvCallback);

	// TODO: global init
	srtp_init();

	PLOG_DEBUG << "Initializing SRTP transport";

	mPolicy = {};
	srtp_crypto_policy_set_rtp_default(&mPolicy.rtp);
	srtp_crypto_policy_set_rtcp_default(&mPolicy.rtcp);
}

DtlsSrtpTransport::~DtlsSrtpTransport() { stop(); }

void DtlsSrtpTransport::stop() {
	Transport::stop();
	onRecv(nullptr);

	// TODO: global cleanup
	srtp_shutdown();
}

bool DtlsSrtpTransport::send(message_ptr message) {
	if (!message)
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();

	// TODO
	return false;
}

void DtlsSrtpTransport::incoming(message_ptr message) {
	//
}

void DtlsSrtpTransport::postHandshake() {
	// TODO: derive keys

	mPolicy.ssrc = mSsrc;
	mPolicy.key = key;
}

} // namespace rtc
