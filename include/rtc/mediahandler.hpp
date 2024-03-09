/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_MEDIA_HANDLER_H
#define RTC_MEDIA_HANDLER_H

#include "common.hpp"
#include "description.hpp"
#include "message.hpp"

namespace rtc {

class RTC_CPP_EXPORT MediaHandler : public std::enable_shared_from_this<MediaHandler> {
public:
	MediaHandler();
	virtual ~MediaHandler();

	/// Called when a media is added or updated
	/// @param desc Description of the media
	virtual void media([[maybe_unused]] const Description::Media &desc) {}

	/// Called when there is traffic coming from the peer
	/// @param messages Incoming messages from the peer, can be modified by the handler
	/// @param send Send callback to send messages back to the peer
	virtual void incoming([[maybe_unused]] message_vector &messages, [[maybe_unused]] const message_callback &send) {}

	/// Called when there is traffic that needs to be sent to the peer
	/// @param messages Outgoing messages to the peer, can be modified by the handler
	/// @param send Send callback to send messages back to the peer
	virtual void outgoing([[maybe_unused]] message_vector &messages, [[maybe_unused]] const message_callback &send) {}

	virtual bool requestKeyframe(const message_callback &send);
	virtual bool requestBitrate(unsigned int bitrate, const message_callback &send);

	void addToChain(shared_ptr<MediaHandler> handler);
	void setNext(shared_ptr<MediaHandler> handler);
	shared_ptr<MediaHandler> next();
	shared_ptr<const MediaHandler> next() const;
	shared_ptr<MediaHandler> last();             // never null
	shared_ptr<const MediaHandler> last() const; // never null

	void mediaChain(const Description::Media &desc);
	void incomingChain(message_vector &messages, const message_callback &send);
	void outgoingChain(message_vector &messages, const message_callback &send);

private:
	shared_ptr<MediaHandler> mNext;
};

} // namespace rtc

#endif // RTC_MEDIA_HANDLER_H
