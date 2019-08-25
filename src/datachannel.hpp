/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#ifndef RTC_DATA_CHANNEL_H
#define RTC_DATA_CHANNEL_H

#include "channel.hpp"
#include "include.hpp"
#include "message.hpp"
#include "reliability.hpp"

#include <chrono>
#include <functional>
#include <variant>

namespace rtc {

class SctpTransport;
class PeerConnection;

class DataChannel : public Channel {
public:
	DataChannel(std::shared_ptr<SctpTransport> sctpTransport, unsigned int streamId);
	DataChannel(std::shared_ptr<SctpTransport> sctpTransport, unsigned int streamId, string label,
	            string protocol, Reliability reliability);
	~DataChannel();

	void close(void);
	void send(const std::variant<binary, string> &data);
	void send(const byte *data, size_t size);

	string label() const;
	string protocol() const;
	Reliability reliability() const;

	bool isOpen(void) const;
	bool isClosed(void) const;

private:
	void open();
	void incoming(message_ptr message);
	void processOpenMessage(message_ptr message);

	const std::shared_ptr<SctpTransport> mSctpTransport;
	const unsigned int mStreamId;

	string mLabel;
	string mProtocol;
	std::shared_ptr<Reliability> mReliability;

	bool mIsOpen = false;
	bool mIsClosed = false;

	friend class PeerConnection;
};

} // namespace rtc

#endif

