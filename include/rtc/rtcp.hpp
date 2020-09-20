/**
 * Copyright (c) 2020 Staz M
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

#ifndef RTC_RTCP_H
#define RTC_RTCP_H

#include "include.hpp"
#include "log.hpp"
#include "message.hpp"

namespace rtc {

typedef uint32_t SSRC;

class RtcpHandler {
public:
	virtual void onOutgoing(std::function<void(rtc::message_ptr)> cb) = 0;
	virtual std::optional<rtc::message_ptr> incoming(rtc::message_ptr ptr) = 0;
};

// An RtcpSession can be plugged into a Track to handle the whole RTCP session
class RtcpSession : public RtcpHandler {
public:
	void onOutgoing(std::function<void(rtc::message_ptr)> cb) override;

	std::optional<rtc::message_ptr> incoming(rtc::message_ptr ptr) override;
	void requestBitrate(unsigned int newBitrate);

private:
	void pushREMB(unsigned int bitrate);
	void pushRR(unsigned int lastSR_delay);
	void tx(message_ptr msg);

	unsigned int mRequestedBitrate = 0;
	synchronized_callback<rtc::message_ptr> mTxCallback;
	SSRC mSsrc = 0;
	uint32_t mGreatestSeqNo = 0;
	uint64_t mSyncRTPTS, mSyncNTPTS;
};

} // namespace rtc

#endif // RTC_RTCP_H
