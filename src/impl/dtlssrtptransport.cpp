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
		throw std::runtime_error("SRTP create failed, status=" + to_string(static_cast<int>(err)));
	}
	if (srtp_err_status_t err = srtp_create(&mSrtpOut, nullptr)) {
		srtp_dealloc(mSrtpIn);
		throw std::runtime_error("SRTP create failed, status=" + to_string(static_cast<int>(err)));
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

	uint8_t value2 = to_integer<uint8_t>(*(message->begin() + 1)) & 0x7F;
	PLOG_VERBOSE << "Demultiplexing SRTCP and SRTP with RTP payload type, value="
	             << unsigned(value2);

	// RFC 5761 Multiplexing RTP and RTCP 4. Distinguishable RTP and RTCP Packets
	// https://tools.ietf.org/html/rfc5761#section-4
	// It is RECOMMENDED to follow the guidelines in the RTP/AVP profile for the choice of RTP
	// payload type values, with the additional restriction that payload type values in the
	// range 64-95 MUST NOT be used. Specifically, dynamic RTP payload types SHOULD be chosen in
	// the range 96-127 where possible. Values below 64 MAY be used if that is insufficient
	// [...]
	if (value2 >= 64 && value2 <= 95) { // Range 64-95 (inclusive) MUST be RTCP
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
		// See https://datatracker.ietf.org/doc/html/rfc8837#section-5
		message->dscp = 36; // AF42: Assured Forwarding class 4, medium drop probability
	}

	return Transport::outgoing(message); // bypass DTLS DSCP marking
}

void DtlsSrtpTransport::incoming(message_ptr message) {
	if (!mInitDone) {
		// Bypas
		DtlsTransport::incoming(message);
		return;
	}

	int size = int(message->size());
	if (size == 0)
		return;

	// RFC 5764 5.1.2. Reception
	// https://tools.ietf.org/html/rfc5764#section-5.1.2
	// The process for demultiplexing a packet is as follows. The receiver looks at the first byte
	// of the packet. [...] If the value is in between 128 and 191 (inclusive), then the packet is
	// RTP (or RTCP [...]). If the value is between 20 and 63 (inclusive), the packet is DTLS.
	uint8_t value1 = to_integer<uint8_t>(*message->begin());
	PLOG_VERBOSE << "Demultiplexing DTLS and SRTP/SRTCP with first byte, value="
	             << unsigned(value1);

	if (value1 >= 20 && value1 <= 63) {
		PLOG_VERBOSE << "Incoming DTLS packet, size=" << size;
		DtlsTransport::incoming(message);

	} else if (value1 >= 128 && value1 <= 191) {
		// The RTP header has a minimum size of 12 bytes
		// An RTCP packet can have a minimum size of 8 bytes
		if (size < 8) {
			COUNTER_MEDIA_TRUNCATED++;
			PLOG_VERBOSE << "Incoming SRTP/SRTCP packet too short, size=" << size;
			return;
		}

		uint8_t value2 = to_integer<uint8_t>(*(message->begin() + 1)) & 0x7F;
		PLOG_VERBOSE << "Demultiplexing SRTCP and SRTP with RTP payload type, value="
		             << unsigned(value2);

		// See RFC 5761 reference above
		if (value2 >= 64 && value2 <= 95) { // Range 64-95 (inclusive) MUST be RTCP
			PLOG_VERBOSE << "Incoming SRTCP packet, size=" << size;
			if (srtp_err_status_t err = srtp_unprotect_rtcp(mSrtpIn, message->data(), &size)) {
				if (err == srtp_err_status_replay_fail) {
					PLOG_VERBOSE << "Incoming SRTCP packet is a replay";
					COUNTER_SRTCP_REPLAY++;
				} else if (err == srtp_err_status_auth_fail) {
					PLOG_VERBOSE << "Incoming SRTCP packet failed authentication check";
					COUNTER_SRTCP_AUTH_FAIL++;
				} else {
					PLOG_VERBOSE << "SRTCP unprotect error, status=" << err;
					COUNTER_SRTCP_FAIL++;
				}

				return;
			}
			PLOG_VERBOSE << "Unprotected SRTCP packet, size=" << size;
			message->type = Message::Control;
			message->stream = reinterpret_cast<RTCP_SR *>(message->data())->senderSSRC();

		} else {
			PLOG_VERBOSE << "Incoming SRTP packet, size=" << size;
			if (srtp_err_status_t err = srtp_unprotect(mSrtpIn, message->data(), &size)) {
				if (err == srtp_err_status_replay_fail) {
					PLOG_VERBOSE << "Incoming SRTP packet is a replay";
					COUNTER_SRTP_REPLAY++;
				} else if (err == srtp_err_status_auth_fail) {
					PLOG_VERBOSE << "Incoming SRTP packet failed authentication check";
					COUNTER_SRTP_AUTH_FAIL++;
				} else {
					PLOG_VERBOSE << "SRTP unprotect error, status=" << err;
					COUNTER_SRTP_FAIL++;
				}
				return;
			}
			PLOG_VERBOSE << "Unprotected SRTP packet, size=" << size;
			message->type = Message::Binary;
			message->stream = reinterpret_cast<RTP *>(message->data())->ssrc();
		}

		message->resize(size);
		mSrtpRecvCallback(message);

	} else {
		COUNTER_UNKNOWN_PACKET_TYPE++;
		PLOG_VERBOSE << "Unknown packet type, value=" << unsigned(value1) << ", size=" << size;
	}
}

void DtlsSrtpTransport::postHandshake() {
	if (mInitDone)
		return;

	static_assert(SRTP_AES_ICM_128_KEY_LEN_WSALT == SRTP_AES_128_KEY_LEN + SRTP_SALT_LEN);

	const size_t materialLen = SRTP_AES_ICM_128_KEY_LEN_WSALT * 2;
	unsigned char material[materialLen];
	const unsigned char *clientKey, *clientSalt, *serverKey, *serverSalt;

#if USE_GNUTLS
	PLOG_INFO << "Deriving SRTP keying material (GnuTLS)";

	gnutls_datum_t clientKeyDatum, clientSaltDatum, serverKeyDatum, serverSaltDatum;
	gnutls::check(gnutls_srtp_get_keys(mSession, material, materialLen, &clientKeyDatum,
	                                   &clientSaltDatum, &serverKeyDatum, &serverSaltDatum),
	              "Failed to derive SRTP keys");

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
	PLOG_INFO << "Deriving SRTP keying material (OpenSSL)";

	// The extractor provides the client write master key, the server write master key, the client
	// write master salt and the server write master salt in that order.
	const string label = "EXTRACTOR-dtls_srtp";

	// returns 1 on success, 0 or -1 on failure (OpenSSL API is a complete mess...)
	if (SSL_export_keying_material(mSsl, material, materialLen, label.c_str(), label.size(),
	                               nullptr, 0, 0) <= 0)
		throw std::runtime_error("Failed to derive SRTP keys: " +
		                         openssl::error_string(ERR_get_error()));

	// Order is client key, server key, client salt, and server salt
	clientKey = material;
	serverKey = clientKey + SRTP_AES_128_KEY_LEN;
	clientSalt = serverKey + SRTP_AES_128_KEY_LEN;
	serverSalt = clientSalt + SRTP_SALT_LEN;
#endif

	std::memcpy(mClientSessionKey, clientKey, SRTP_AES_128_KEY_LEN);
	std::memcpy(mClientSessionKey + SRTP_AES_128_KEY_LEN, clientSalt, SRTP_SALT_LEN);

	std::memcpy(mServerSessionKey, serverKey, SRTP_AES_128_KEY_LEN);
	std::memcpy(mServerSessionKey + SRTP_AES_128_KEY_LEN, serverSalt, SRTP_SALT_LEN);

	srtp_policy_t inbound = {};
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&inbound.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&inbound.rtcp);
	inbound.ssrc.type = ssrc_any_inbound;
	inbound.key = mIsClient ? mServerSessionKey : mClientSessionKey;
	inbound.window_size = 1024;
	inbound.allow_repeat_tx = true;
	inbound.next = nullptr;

	if (srtp_err_status_t err = srtp_add_stream(mSrtpIn, &inbound))
		throw std::runtime_error("SRTP add inbound stream failed, status=" +
		                         to_string(static_cast<int>(err)));

	srtp_policy_t outbound = {};
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&outbound.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&outbound.rtcp);
	outbound.ssrc.type = ssrc_any_outbound;
	outbound.key = mIsClient ? mClientSessionKey : mServerSessionKey;
	outbound.window_size = 1024;
	outbound.allow_repeat_tx = true;
	outbound.next = nullptr;

	if (srtp_err_status_t err = srtp_add_stream(mSrtpOut, &outbound))
		throw std::runtime_error("SRTP add outbound stream failed, status=" +
		                         to_string(static_cast<int>(err)));

	mInitDone = true;
}

} // namespace rtc::impl

#endif
