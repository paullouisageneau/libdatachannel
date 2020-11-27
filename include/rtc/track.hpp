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
#include "description.hpp"
#include "include.hpp"
#include "message.hpp"
#include "queue.hpp"
#include "rtcp.hpp"

#include <atomic>
#include <variant>

namespace rtc {

#if RTC_ENABLE_MEDIA
class DtlsSrtpTransport;
#endif

class RTC_CPP_EXPORT Track final : public std::enable_shared_from_this<Track>, public Channel {
public:
	Track(Description::Media description);
	~Track() = default;

	string mid() const;
	Description::Media description() const;

	void setDescription(Description::Media description);

	void close(void) override;
	bool send(message_variant data) override;
	bool send(const byte *data, size_t size) override;

	bool isOpen(void) const override;
	bool isClosed(void) const override;
	size_t maxMessageSize() const override;

	// Extended API
	size_t availableAmount() const override;
	std::optional<message_variant> receive() override;
	std::optional<message_variant> peek() override;

	bool requestKeyframe();

	// RTCP handler
	void setRtcpHandler(std::shared_ptr<RtcpHandler> handler);
	std::shared_ptr<RtcpHandler> getRtcpHandler();

private:
#if RTC_ENABLE_MEDIA
	void open(std::shared_ptr<DtlsSrtpTransport> transport);
	std::weak_ptr<DtlsSrtpTransport> mDtlsSrtpTransport;
#endif

	void incoming(message_ptr message);
	bool outgoing(message_ptr message);

	Description::Media mMediaDescription;
	std::atomic<bool> mIsClosed = false;

	Queue<message_ptr> mRecvQueue;
	std::shared_ptr<RtcpHandler> mRtcpHandler;

	friend class PeerConnection;
};

} // namespace rtc

#endif
