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

#ifndef RTC_DTLS_SRTP_TRANSPORT_H
#define RTC_DTLS_SRTP_TRANSPORT_H

#include "dtlstransport.hpp"
#include "include.hpp"

#include <srtp2/srtp.h>

namespace rtc {

class DtlsSrtpTransport final : public DtlsTransport {
public:
	DtlsSrtpTransport(std::shared_ptr<IceTransport> lower, std::shared_ptr<Certificate> certificate,
	                  verifier_callback verifierCallback, message_callback recvCallback,
	                  state_callback stateChangeCallback);
	~DtlsSrtpTransport();

	bool stop() override;
	bool send(message_ptr message) override;

private:
	void incoming(message_ptr message) override;
	void postHandshake() override;

	message_callback mRecvCallback;

	srtp_t mSrtp;
};

} // namespace rtc

#endif
