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

	DtlsSrtpTransport(shared_ptr<IceTransport> lower, certificate_ptr certificate,
	                  optional<size_t> mtu, verifier_callback verifierCallback,
	                  message_callback srtpRecvCallback, state_callback stateChangeCallback);
	~DtlsSrtpTransport();

	bool sendMedia(message_ptr message);

private:
	void incoming(message_ptr message) override;
	void postHandshake() override;

	message_callback mSrtpRecvCallback;

	srtp_t mSrtpIn, mSrtpOut;

	std::atomic<bool> mInitDone = false;
	unsigned char mClientSessionKey[SRTP_AES_ICM_128_KEY_LEN_WSALT];
	unsigned char mServerSessionKey[SRTP_AES_ICM_128_KEY_LEN_WSALT];
	std::mutex sendMutex;
};

} // namespace rtc::impl

#endif

#endif
