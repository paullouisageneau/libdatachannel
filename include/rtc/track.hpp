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

#ifndef RTC_TRACK_H
#define RTC_TRACK_H

#include "channel.hpp"
#include "common.hpp"
#include "description.hpp"
#include "mediahandler.hpp"
#include "message.hpp"

namespace rtc {

namespace impl {

class Track;

} // namespace impl

class RTC_CPP_EXPORT Track final : private CheshireCat<impl::Track>, public Channel {
public:
	Track(impl_ptr<impl::Track> impl);
	~Track() = default;

	string mid() const;
	Description::Direction direction() const;
	Description::Media description() const;

	void setDescription(Description::Media description);

	void close(void) override;
	bool send(message_variant data) override;
	bool send(const byte *data, size_t size) override;

	bool isOpen(void) const override;
	bool isClosed(void) const override;
	size_t maxMessageSize() const override;

	bool requestKeyframe();

	void setMediaHandler(shared_ptr<MediaHandler> handler);
	shared_ptr<MediaHandler> getMediaHandler();

	// Deprecated, use setMediaHandler() and getMediaHandler()
	inline void setRtcpHandler(shared_ptr<MediaHandler> handler) { setMediaHandler(handler); }
	inline shared_ptr<MediaHandler> getRtcpHandler() { return getMediaHandler(); }

private:
	using CheshireCat<impl::Track>::impl;
};

} // namespace rtc

#endif
