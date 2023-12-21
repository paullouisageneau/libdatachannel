/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_DTLS_SRTP_TRANSPORT_H
#define RTC_IMPL_DTLS_SRTP_TRANSPORT_H

#include "common.hpp"
#include "dtlstransport.hpp"

#if RTC_ENABLE_MEDIA

#if RTC_SYSTEM_SRTP
#include <srtp2/srtp.h>
#else
#include "srtp.h"
#endif

#include <atomic>

namespace rtc::impl {

class DtlsSrtpTransport final : public DtlsTransport {
public:
	static void Init();
	static void Cleanup();
	static bool IsGcmSupported();

	DtlsSrtpTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
	                  optional<size_t> mtu, CertificateFingerprint::Algorithm fingerprintAlgorithm,
	                  verifier_callback verifierCallback, message_callback srtpRecvCallback,
	                  state_callback stateChangeCallback);
	~DtlsSrtpTransport();

	bool sendMedia(message_ptr message);

private:
	void recvMedia(message_ptr message);
	bool demuxMessage(message_ptr message) override;
	void postHandshake() override;

#if !USE_GNUTLS && !USE_MBEDTLS
	struct ProfileParams {
		srtp_profile_t srtpProfile;
		size_t keySize;
		size_t saltSize;
	};

	ProfileParams getProfileParamsFromName(string_view name);
#endif

	message_callback mSrtpRecvCallback;
	srtp_t mSrtpIn, mSrtpOut;
	std::atomic<bool> mInitDone = false;
	std::vector<unsigned char> mClientSessionKey;
	std::vector<unsigned char> mServerSessionKey;
	std::mutex sendMutex;
};

} // namespace rtc::impl

#endif

#endif
