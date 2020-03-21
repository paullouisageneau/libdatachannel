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

#include <cstring>
#include <exception>

#include <srtp2/srtp.h>

using std::shared_ptr;
using std::to_string;

namespace rtc {

void DtlsSrtpTransport::Init() { srtp_init(); }

void DtlsSrtpTransport::Cleanup() { srtp_shutdown(); }

DtlsSrtpTransport::DtlsSrtpTransport(std::shared_ptr<IceTransport> lower,
                                     shared_ptr<Certificate> certificate,
                                     verifier_callback verifierCallback,
                                     message_callback srtpRecvCallback,
                                     state_callback stateChangeCallback)
    : DtlsTransport(lower, certificate, std::move(verifierCallback),
                    std::move(stateChangeCallback)),
      mSrtpRecvCallback(std::move(srtpRecvCallback)) { // distinct from Transport recv callback

	PLOG_DEBUG << "Initializing SRTP transport";

#if USE_GNUTLS
	// TODO: check_gnutls
	gnutls_srtp_set_profile(mSession, GNUTLS_SRTP_AES128_CM_HMAC_SHA1_80);
#else
	// TODO: check_openssl
	SSL_set_tlsext_use_srtp(mSsl, "SRTP_AES128_CM_SHA1_80");
#endif
}

DtlsSrtpTransport::~DtlsSrtpTransport() { stop(); }

bool DtlsSrtpTransport::stop() {
	if (!Transport::stop())
		return false;

	srtp_dealloc(mSrtp);
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
	mSrtpRecvCallback(message);
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

	const size_t materialLen = SRTP_AES_ICM_128_KEY_LEN_WSALT * 2;
	unsigned char material[materialLen];
	const unsigned char *clientKey, *clientSalt, *serverKey, *serverSalt;

#if USE_GNUTLS
	gnutls_datum_t clientKeyDatum, clientSaltDatum, serverKeyDatum, serverSaltDatum;
	// TODO: check_gnutls
	gnutls_srtp_get_keys(mSession, material, materialLen, &clientKeyDatum, &clientSaltDatum,
	                     &serverKeyDatum, &serverSaltDatum);

	if (clientKeyDatum.size != SRTP_AES_128_KEY_LEN)
		throw std::logic_error("Unexpected SRTP master key length: " +
		                       to_string(clientKeyDatum.size));
	if (clientSaltDatum.size != SRTP_SALT_LEN)
		throw std::logic_error("Unexpected SRTP salt length: " + to_string(clientSaltDatum.size));
	if (serverKeyDatum.size != SRTP_AES_128_KEY_LEN)
		throw std::logic_error("Unexpected SRTP master key length: " +
		                       to_string(serverKeyDatum.size));
	if (serverSaltDatum.size != SRTP_SALT_LEN)
		throw std::logic_error("Unexpected SRTP salt size: " + to_string(serverSaltDatum.size));

	clientKey = reinterpret_cast<const unsigned char *>(clientKeyDatum.data);
	clientSalt = reinterpret_cast<const unsigned char *>(clientSaltDatum.data);

	serverKey = reinterpret_cast<const unsigned char *>(serverKeyDatum.data);
	serverSalt = reinterpret_cast<const unsigned char *>(serverSaltDatum.data);
#else
	// This provides the client write master key, the server write master key, the client write
	// master salt and the server write master salt in that order.
	const string label = "EXTRACTOR-dtls_srtp";
	// TODO: check OpenSSL
	SSL_export_keying_material(mSsl, material, SRTP_MAX_KEY_LEN * 2, label.c_str(), label.size(),
	                           nullptr, 0, 0);
	clientKey = material;
	clientSalt = clientKey + SRTP_AES_128_KEY_LEN;

	serverKey = material + SRTP_AES_ICM_128_KEY_LEN_WSALT;
	serverSalt = serverSalt + SRTP_AES_128_KEY_LEN;
#endif

	unsigned char clientSessionKey[SRTP_AES_ICM_128_KEY_LEN_WSALT];
	std::memcpy(clientSessionKey, clientKey, SRTP_AES_128_KEY_LEN);
	std::memcpy(clientSessionKey + SRTP_AES_128_KEY_LEN, clientSalt, SRTP_SALT_LEN);

	unsigned char serverSessionKey[SRTP_AES_ICM_128_KEY_LEN_WSALT];
	std::memcpy(serverSessionKey, serverKey, SRTP_AES_128_KEY_LEN);
	std::memcpy(serverSessionKey + SRTP_AES_128_KEY_LEN, serverSalt, SRTP_SALT_LEN);

	if (mIsClient) {
		inbound.key = serverSessionKey;
		outbound.key = clientSessionKey;
	} else {
		inbound.key = clientSessionKey;
		outbound.key = serverSessionKey;
	}

	srtp_policy_t *policies = &inbound;
	inbound.next = &outbound;
	outbound.next = nullptr;

	if (srtp_err_status_t err = srtp_create(&mSrtp, policies))
		throw std::runtime_error("SRTP create failed, status=" + to_string(static_cast<int>(err)));
}

} // namespace rtc
