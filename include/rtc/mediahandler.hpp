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

#ifndef RTC_MEDIA_HANDLER_H
#define RTC_MEDIA_HANDLER_H

#include "common.hpp"
#include "message.hpp"

namespace rtc {

class RTC_CPP_EXPORT MediaHandler {
protected:
	// Use this callback when trying to send custom data (such as RTCP) to the client.
	synchronized_callback<message_ptr> outgoingCallback;

public:
	// Called when there is traffic coming from the peer
	virtual message_ptr incoming(message_ptr ptr) = 0;

	// Called when there is traffic that needs to be sent to the peer
	virtual message_ptr outgoing(message_ptr ptr) = 0;

	// This callback is used to send traffic back to the peer.
	void onOutgoing(const std::function<void(message_ptr)> &cb) {
		this->outgoingCallback = synchronized_callback<message_ptr>(cb);
	}

	virtual bool requestKeyframe() { return false; }
};

} // namespace rtc

#endif // RTC_MEDIA_HANDLER_H
