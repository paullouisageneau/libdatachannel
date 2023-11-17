/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dtlssrtptransport.hpp"
#include "logcounter.hpp"
#include "rtp.hpp"
#include "tls.hpp"

#if RTC_ENABLE_MEDIA

#include <cstring>
#include <exception>

using std::to_integer;
using std::to_string;

namespace rtc::impl {

static LogCounter COUNTER_MEDIA_TRUNCATED(plog::warning,
                                          "Number of truncated SRT(C)P packets received");
static LogCounter
    COUNTER_UNKNOWN_PACKET_TYPE(plog::warning,
                                "Number of RTP packets received with an unknown packet type");
static LogCounter COUNTER_SRTCP_REPLAY(plog::warning, "Number of SRTCP replay packets received");
static LogCounter
    COUNTER_SRTCP_AUTH_FAIL(plog::warning,
                            "Number of SRTCP packets received that failed authentication checks");
static LogCounter
    COUNTER_SRTCP_FAIL(plog::warning,
                       "Number of SRTCP packets received that had an unknown libSRTP failure");
static LogCounter COUNTER_SRTP_REPLAY(plog::warning, "Number of SRTP replay packets received");
static LogCounter
    COUNTER_SRTP_AUTH_FAIL(plog::warning,
                           "Number of SRTP packets received that failed authentication checks");
static LogCounter
    COUNTER_SRTP_FAIL(plog::warning,
                      "Number of SRTP packets received that had an unknown libSRTP failure");

void DtlsSrtpTransport::Init() { srtp_init(); }

void DtlsSrtpTransport::Cleanup() { srtp_shutdown(); }

bool DtlsSrtpTransport::IsGcmSupported() {
#if RTC_SYSTEM_SRTP
	// system libSRTP may not have GCM support
	srtp_policy_t policy = {};
	return srtp_crypto_policy_set_from_profile_for_rtp(
	           &policy.rtp, srtp_profile_aead_aes_256_gcm) == srtp_err_status_ok;
#else
	return true;
#endif
}

DtlsSrtpTransport::DtlsSrtpTransport(shared_ptr<IceTransport> lower,
                                     shared_ptr<Certificate> certificate, optional<size_t> mtu,
                                     verifier_callback verifierCallback,
                                     message_callback srtpRecvCallback,
                                     state_callback stateChangeCallback)
    : DtlsTransport(lower, certificate, mtu, std::move(verifierCallback),
                    std::move(stateChangeCallback)),
      mSrtpRecvCallback(std::move(srtpRecvCallback)) { // distinct from Transport recv callback

	PLOG_DEBUG << "Initializing DTLS-SRTP transport";

	if (srtp_err_status_t err = srtp_create(&mSrtpIn, nullptr)) {
		throw std::runtime_error("srtp_create failed, status=" + to_string(static_cast<int>(err)));
	}
	if (srtp_err_status_t err = srtp_create(&mSrtpOut, nullptr)) {
		srtp_dealloc(mSrtpIn);
		throw std::runtime_error("srtp_create failed, status=" + to_string(static_cast<int>(err)));
	}
}

DtlsSrtpTransport::~DtlsSrtpTransport() {
	stop(); // stop before deallocating

	srtp_dealloc(mSrtpIn);
	srtp_dealloc(mSrtpOut);
}

bool DtlsSrtpTransport::sendMedia(message_ptr message) {
	std::lock_guard lock(sendMutex);
	if (!message)
		return false;

	if (!mInitDone) {
		PLOG_ERROR << "SRTP media sent before keys are derived";
		return false;
	}

	int size = int(message->size());
	PLOG_VERBOSE << "Send size=" << size;

	// The RTP header has a minimum size of 12 bytes
	// An RTCP packet can have a minimum size of 8 bytes
	if (size < 8)
		throw std::runtime_error("RTP/RTCP packet too short");

	// srtp_protect() and srtp_protect_rtcp() assume that they can write SRTP_MAX_TRAILER_LEN (for
	// the authentication tag) into the location in memory immediately following the RTP packet.
	message->resize(size + SRTP_MAX_TRAILER_LEN);

	if (IsRtcp(*message)) { // Demultiplex RTCP and RTP using payload type
		if (srtp_err_status_t err = srtp_protect_rtcp(mSrtpOut, message->data(), &size)) {
			if (err == srtp_err_status_replay_fail)
				throw std::runtime_error("Outgoing SRTCP packet is a replay");
			else
				throw std::runtime_error("SRTCP protect error, status=" +
				                         to_string(static_cast<int>(err)));
		}
		PLOG_VERBOSE << "Protected SRTCP packet, size=" << size;

	} else {
		if (srtp_err_status_t err = srtp_protect(mSrtpOut, message->data(), &size)) {
			if (err == srtp_err_status_replay_fail)
				throw std::runtime_error("Outgoing SRTP packet is a replay");
			else
				throw std::runtime_error("SRTP protect error, status=" +
				                         to_string(static_cast<int>(err)));
		}
		PLOG_VERBOSE << "Protected SRTP packet, size=" << size;
	}

	message->resize(size);

	if (message->dscp == 0) { // Track might override the value
		// Set recommended medium-priority DSCP value
		// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
		message->dscp = 36; // AF42: Assured Forwarding class 4, medium drop probability
	}

	return Transport::outgoing(message); // bypass DTLS DSCP marking
}

void DtlsSrtpTransport::recvMedia(message_ptr message) {
	// The RTP header has a minimum size of 12 bytes
	// An RTCP packet can have a minimum size of 8 bytes
	int size = int(message->size());
	if (size < 8) {
		COUNTER_MEDIA_TRUNCATED++;
		PLOG_VERBOSE << "Incoming SRTP/SRTCP packet too short, size=" << size;
		return;
	}

	uint8_t value2 = to_integer<uint8_t>(*(message->begin() + 1)) & 0x7F;
	PLOG_VERBOSE << "Demultiplexing SRTCP and SRTP with RTP payload type, value="
	             << unsigned(value2);

	if (IsRtcp(*message)) { // Demultiplex RTCP and RTP using payload type
		PLOG_VERBOSE << "Incoming SRTCP packet, size=" << size;
		if (srtp_err_status_t err = srtp_unprotect_rtcp(mSrtpIn, message->data(), &size)) {
			if (err == srtp_err_status_replay_fail) {
				PLOG_VERBOSE << "Incoming SRTCP packet is a replay";
				COUNTER_SRTCP_REPLAY++;
			} else if (err == srtp_err_status_auth_fail) {
				PLOG_DEBUG << "Incoming SRTCP packet failed authentication check";
				COUNTER_SRTCP_AUTH_FAIL++;
			} else {
				PLOG_DEBUG << "SRTCP unprotect error, status=" << err;
				COUNTER_SRTCP_FAIL++;
			}

			return;
		}
		PLOG_VERBOSE << "Unprotected SRTCP packet, size=" << size;
		message->type = Message::Control;
		message->stream = reinterpret_cast<RtcpSr *>(message->data())->senderSSRC();

	} else {
		PLOG_VERBOSE << "Incoming SRTP packet, size=" << size;
		if (srtp_err_status_t err = srtp_unprotect(mSrtpIn, message->data(), &size)) {
			if (err == srtp_err_status_replay_fail) {
				PLOG_VERBOSE << "Incoming SRTP packet is a replay";
				COUNTER_SRTP_REPLAY++;
			} else if (err == srtp_err_status_auth_fail) {
				PLOG_DEBUG << "Incoming SRTP packet failed authentication check";
				COUNTER_SRTP_AUTH_FAIL++;
			} else {
				PLOG_DEBUG << "SRTP unprotect error, status=" << err;
				COUNTER_SRTP_FAIL++;
			}
			return;
		}
		PLOG_VERBOSE << "Unprotected SRTP packet, size=" << size;
		message->type = Message::Binary;
		message->stream = reinterpret_cast<RtpHeader *>(message->data())->ssrc();
	}

	message->resize(size);
	mSrtpRecvCallback(message);
}

bool DtlsSrtpTransport::demuxMessage(message_ptr message) {
	if (!mInitDone) {
		// Bypass
		return false;
	}

	if (message->size() == 0)
		return false;

	// RFC 5764 5.1.2. Reception
	// https://www.rfc-editor.org/rfc/rfc5764.html#section-5.1.2
	// The process for demultiplexing a packet is as follows. The receiver looks at the first byte
	// of the packet. [...] If the value is in between 128 and 191 (inclusive), then the packet is
	// RTP (or RTCP [...]). If the value is between 20 and 63 (inclusive), the packet is DTLS.
	uint8_t value1 = to_integer<uint8_t>(*message->begin());
	PLOG_VERBOSE << "Demultiplexing DTLS and SRTP/SRTCP with first byte, value="
	             << unsigned(value1);

	if (value1 >= 20 && value1 <= 63) {
		PLOG_VERBOSE << "Incoming DTLS packet, size=" << message->size();
		return false;

	} else if (value1 >= 128 && value1 <= 191) {
		recvMedia(std::move(message));
		return true;

	} else {
		COUNTER_UNKNOWN_PACKET_TYPE++;
		PLOG_DEBUG << "Unknown packet type, value=" << unsigned(value1)
		           << ", size=" << message->size();
		return true;
	}
}

void DtlsSrtpTransport::postHandshake() {
	if (mInitDone)
		return;

#if USE_GNUTLS
	PLOG_INFO << "Deriving SRTP keying material (GnuTLS)";

	const srtp_profile_t srtpProfile = srtp_profile_aes128_cm_sha1_80;
	const size_t keySize = SRTP_AES_128_KEY_LEN;
	const size_t saltSize = SRTP_SALT_LEN;
	const size_t keySizeWithSalt = SRTP_AES_ICM_128_KEY_LEN_WSALT;

	const size_t materialLen = keySizeWithSalt * 2;
	std::vector<unsigned char> material(materialLen);
	gnutls_datum_t clientKeyDatum, clientSaltDatum, serverKeyDatum, serverSaltDatum;
	gnutls::check(gnutls_srtp_get_keys(mSession, material.data(), materialLen, &clientKeyDatum,
	                                   &clientSaltDatum, &serverKeyDatum, &serverSaltDatum),
	              "Failed to derive SRTP keys");

	if (clientKeyDatum.size != keySize)
		throw std::logic_error("Unexpected SRTP master key length: " +
		                       to_string(clientKeyDatum.size));
	if (clientSaltDatum.size != saltSize)
		throw std::logic_error("Unexpected SRTP salt length: " + to_string(clientSaltDatum.size));
	if (serverKeyDatum.size != keySize)
		throw std::logic_error("Unexpected SRTP master key length: " +
		                       to_string(serverKeyDatum.size));
	if (serverSaltDatum.size != saltSize)
		throw std::logic_error("Unexpected SRTP salt size: " + to_string(serverSaltDatum.size));

	const unsigned char *clientKey = reinterpret_cast<const unsigned char *>(clientKeyDatum.data);
	const unsigned char *clientSalt = reinterpret_cast<const unsigned char *>(clientSaltDatum.data);
	const unsigned char *serverKey = reinterpret_cast<const unsigned char *>(serverKeyDatum.data);
	const unsigned char *serverSalt = reinterpret_cast<const unsigned char *>(serverSaltDatum.data);

#elif USE_MBEDTLS
	PLOG_INFO << "Deriving SRTP keying material (Mbed TLS)";

	mbedtls_dtls_srtp_info srtpInfo;
	mbedtls_ssl_get_dtls_srtp_negotiation_result(&mSsl, &srtpInfo);
	if (srtpInfo.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80)
		throw std::runtime_error("Failed to get SRTP profile");

	const srtp_profile_t srtpProfile = srtp_profile_aes128_cm_sha1_80;
	const size_t keySize = SRTP_AES_128_KEY_LEN;
	const size_t saltSize = SRTP_SALT_LEN;
	const size_t keySizeWithSalt = SRTP_AES_ICM_128_KEY_LEN_WSALT;

	if (mTlsProfile == MBEDTLS_SSL_TLS_PRF_NONE)
		throw std::logic_error("TLS PRF type is not set");

	// The extractor provides the client write master key, the server write master key, the client
	// write master salt and the server write master salt in that order.
	const string label = "EXTRACTOR-dtls_srtp";
	const size_t materialLen = keySizeWithSalt * 2;
	std::vector<unsigned char> material(materialLen);

	if (mbedtls_ssl_tls_prf(mTlsProfile, reinterpret_cast<const unsigned char *>(mMasterSecret), 48,
	                        label.c_str(), reinterpret_cast<const unsigned char *>(mRandBytes), 64,
	                        material.data(), materialLen) != 0)
		throw std::runtime_error("Failed to derive SRTP keys");

	// Order is client key, server key, client salt, and server salt
	const unsigned char *clientKey = material.data();
	const unsigned char *serverKey = clientKey + keySize;
	const unsigned char *clientSalt = serverKey + keySize;
	const unsigned char *serverSalt = clientSalt + saltSize;

#else // OpenSSL
	PLOG_INFO << "Deriving SRTP keying material (OpenSSL)";
	auto profile = SSL_get_selected_srtp_profile(mSsl);
	if (!profile)
		throw std::runtime_error("Failed to get SRTP profile: " +
		                         openssl::error_string(ERR_get_error()));

	PLOG_DEBUG << "SRTP profile is: " << profile->name;

	const auto [srtpProfile, keySize, saltSize] = getProfileParamsFromName(profile->name);
	const size_t keySizeWithSalt = keySize + saltSize;

	// The extractor provides the client write master key, the server write master key, the client
	// write master salt and the server write master salt in that order.
	const string label = "EXTRACTOR-dtls_srtp";
	const size_t materialLen = keySizeWithSalt * 2;
	std::vector<unsigned char> material(materialLen);

	// returns 1 on success, 0 or -1 on failure (OpenSSL API is a complete mess...)
	if (SSL_export_keying_material(mSsl, material.data(), materialLen, label.c_str(), label.size(),
	                               nullptr, 0, 0) <= 0)
		throw std::runtime_error("Failed to derive SRTP keys: " +
		                         openssl::error_string(ERR_get_error()));

	// Order is client key, server key, client salt, and server salt
	const unsigned char *clientKey = material.data();
	const unsigned char *serverKey = clientKey + keySize;
	const unsigned char *clientSalt = serverKey + keySize;
	const unsigned char *serverSalt = clientSalt + saltSize;
#endif

	mClientSessionKey.resize(keySizeWithSalt);
	mServerSessionKey.resize(keySizeWithSalt);
	std::memcpy(mClientSessionKey.data(), clientKey, keySize);
	std::memcpy(mClientSessionKey.data() + keySize, clientSalt, saltSize);

	std::memcpy(mServerSessionKey.data(), serverKey, keySize);
	std::memcpy(mServerSessionKey.data() + keySize, serverSalt, saltSize);

	srtp_policy_t inbound = {};
	if (srtp_crypto_policy_set_from_profile_for_rtp(&inbound.rtp, srtpProfile))
		throw std::runtime_error("SRTP profile is not supported");
	if (srtp_crypto_policy_set_from_profile_for_rtcp(&inbound.rtcp, srtpProfile))
		throw std::runtime_error("SRTP profile is not supported");

	inbound.ssrc.type = ssrc_any_inbound;
	inbound.key = mIsClient ? mServerSessionKey.data() : mClientSessionKey.data();
	inbound.window_size = 1024;
	inbound.allow_repeat_tx = true;
	inbound.next = nullptr;

	if (srtp_err_status_t err = srtp_add_stream(mSrtpIn, &inbound))
		throw std::runtime_error("SRTP add inbound stream failed, status=" +
		                         to_string(static_cast<int>(err)));

	srtp_policy_t outbound = {};
	if (srtp_crypto_policy_set_from_profile_for_rtp(&outbound.rtp, srtpProfile))
		throw std::runtime_error("SRTP profile is not supported");
	if (srtp_crypto_policy_set_from_profile_for_rtcp(&outbound.rtcp, srtpProfile))
		throw std::runtime_error("SRTP profile is not supported");

	outbound.ssrc.type = ssrc_any_outbound;
	outbound.key = mIsClient ? mClientSessionKey.data() : mServerSessionKey.data();
	outbound.window_size = 1024;
	outbound.allow_repeat_tx = true;
	outbound.next = nullptr;

	if (srtp_err_status_t err = srtp_add_stream(mSrtpOut, &outbound))
		throw std::runtime_error("SRTP add outbound stream failed, status=" +
		                         to_string(static_cast<int>(err)));

	mInitDone = true;
}

#if !USE_GNUTLS && !USE_MBEDTLS
DtlsSrtpTransport::ProfileParams DtlsSrtpTransport::getProfileParamsFromName(string_view name) {
	if (name == "SRTP_AES128_CM_SHA1_80")
		return {srtp_profile_aes128_cm_sha1_80, SRTP_AES_128_KEY_LEN, SRTP_SALT_LEN};
	if (name == "SRTP_AES128_CM_SHA1_32")
		return {srtp_profile_aes128_cm_sha1_32, SRTP_AES_128_KEY_LEN, SRTP_SALT_LEN};
	if (name == "SRTP_AEAD_AES_128_GCM")
		return {srtp_profile_aead_aes_128_gcm, SRTP_AES_128_KEY_LEN, SRTP_AEAD_SALT_LEN};
	if (name == "SRTP_AEAD_AES_256_GCM")
		return {srtp_profile_aead_aes_256_gcm, SRTP_AES_256_KEY_LEN, SRTP_AEAD_SALT_LEN};

	throw std::logic_error("Unknown SRTP profile name: " + std::string(name));
}
#endif

} // namespace rtc::impl

#endif
