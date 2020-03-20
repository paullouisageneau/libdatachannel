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
#include <srtp2/srtp.h>

using std::shared_ptr;
using std::to_string;

namespace rtc {

DtlsSrtpTransport::DtlsSrtpTransport(std::shared_ptr<IceTransport> lower,
                                     shared_ptr<Certificate> certificate,
                                     verifier_callback verifierCallback,
                                     message_callback recvCallback,
                                     state_callback stateChangeCallback)
    : DtlsTransport(lower, certificate, std::move(verifierCallback),
                    std::move(stateChangeCallback)),
      mRecvCallback(std::move(recvCallback)) {

	// TODO: global init
	srtp_init();

	PLOG_DEBUG << "Initializing SRTP transport";

#if USE_GNUTLS
	// TODO: check_gnutls
	gnutls_srtp_set_profile(mSession, GNUTLS_SRTP_AES128_CM_HMAC_SHA1_80);
#else
	// TODO
#endif
}

DtlsSrtpTransport::~DtlsSrtpTransport() { stop(); }

bool DtlsSrtpTransport::stop() {
	if (!Transport::stop())
		return false;

	// TODO: global cleanup
	srtp_shutdown();
	return true;
}

bool DtlsSrtpTransport::send(message_ptr message) {
	if (!message)
		return false;

	int size = message->size();
	PLOG_VERBOSE << "Send size=" << size;

	// srtp_protect() assumes that it can write SRTP_MAX_TRAILER_LEN (for the authentication tag)
	// into the location in memory immediately following the RTP packet.
	message->resize(size + SRTP_MAX_TRAILER_LEN);
	if (srtp_err_status_t err = srtp_protect(mSrtp, message->data(), &size)) {
		if (err == srtp_err_status_replay_fail)
			throw std::runtime_error("SRTP packet is a replay");
		else
			throw std::runtime_error("SRTP protect error");
	}
	PLOG_VERBOSE << "Protected SRTP packet, size=" << size;
	message->resize(size);
	outgoing(message);
	return true;
}

void DtlsSrtpTransport::incoming(message_ptr message) {
	// TODO: demultiplexing
	// detect dtls and pass to DtlsTransport::incoming

	int size = message->size();
	PLOG_VERBOSE << "Incoming SRTP packet, size=" << size;

	if (srtp_err_status_t err = srtp_unprotect(mSrtp, message->data(), &size)) {
		if (err == srtp_err_status_replay_fail)
			PLOG_WARNING << "Incoming SRTP packet is a replay";
		else
			PLOG_WARNING << "SRTP unprotect error, status=" << err;
		return;
	}
	PLOG_VERBOSE << "Unprotected SRTP packet, size=" << size;
	message->resize(size);
	mRecvCallback(message);
}

void DtlsSrtpTransport::postHandshake() {
	srtp_policy_t inbound = {};
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&inbound.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&inbound.rtcp);
	inbound.ssrc.type = ssrc_any_inbound;

	srtp_policy_t outbound = {};
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&outbound.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&outbound.rtcp);
	outbound.ssrc.type = ssrc_any_outbound;

#if USE_GNUTLS
	unsigned char material[SRTP_MAX_KEY_LEN * 2];
	gnutls_datum_t clientKey, clientSalt, serverKey, serverSalt;
	// TODO: check_gnutls
	gnutls_srtp_get_keys(mSession, material, STRP_MAX_KEY_LEN * 2, &clientKey, &clientSalt,
	                     &serverKey, &serverSalt);

	unsigned char clientSessionKey[SRTP_MAX_KEY_LEN];
	std::memcpy(clientSessionKey, clientKey.data, clientKey.size);
	std::memcpy(clientSessionKey + clientKey.size, clientSalt.data, clientSalt.size);

	unsigned char serverSessionKey[SRTP_MAX_KEY_LEN];
	std::memcpy(serverSessionKey, serverKey.data, serverKey.size);
	std::memcpy(serverSessionKey + serverKey.size, serverSalt.data, serverSalt.size);

	if (mIsClient) {
		inbound.key = serverSessionKey;
		outbound.key = clientSessionKey;
	} else {
		inbound.key = clientSessionKey;
		outbound.key = serverSessionKey;
	}
#else
	// TODO
#endif

	srtp_policy_t *policies = &inbound;
	inbound.next = &outbound;
	outbound.next = nullptr;

	if (srtp_err_status_t err = srtp_create(&mSrtp, policies))
		throw std::runtime_error("SRTP create failed, status=" + to_string(static_cast<int>(err)));
}

} // namespace rtc
