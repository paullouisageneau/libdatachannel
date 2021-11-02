/**
 * Copyright (c) 2020 Staz Modrzynski
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

#ifndef RTC_RTCP_RECEIVING_SESSION_H
#define RTC_RTCP_RECEIVING_SESSION_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "mediahandler.hpp"
#include "rtp.hpp"

namespace rtc {

// An RtcpSession can be plugged into a Track to handle the whole RTCP session
class RTC_CPP_EXPORT RtcpReceivingSession : public MediaHandler {
public:
	message_ptr incoming(message_ptr ptr) override;
	message_ptr outgoing(message_ptr ptr) override;
	bool send(message_ptr ptr);

	void requestBitrate(unsigned int newBitrate);

	bool requestKeyframe() override;

protected:
	void pushREMB(unsigned int bitrate);
	void pushRR(unsigned int lastSR_delay);

	void pushPLI();

	unsigned int mRequestedBitrate = 0;
	SSRC mSsrc = 0;
	uint32_t mGreatestSeqNo = 0;
	uint64_t mSyncRTPTS, mSyncNTPTS;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_RTCP_RECEIVING_SESSION_H
