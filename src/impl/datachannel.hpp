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

#ifndef RTC_IMPL_DATA_CHANNEL_H
#define RTC_IMPL_DATA_CHANNEL_H

#include "channel.hpp"
#include "common.hpp"
#include "message.hpp"
#include "peerconnection.hpp"
#include "queue.hpp"
#include "reliability.hpp"
#include "sctptransport.hpp"

#include <atomic>
#include <shared_mutex>

namespace rtc::impl {

struct PeerConnection;

struct DataChannel : Channel, std::enable_shared_from_this<DataChannel> {
	DataChannel(weak_ptr<PeerConnection> pc, uint16_t stream, string label, string protocol,
	            Reliability reliability);
	virtual ~DataChannel();

	void close();
	void remoteClose();
	bool outgoing(message_ptr message);
	void incoming(message_ptr message);

	optional<message_variant> receive() override;
	optional<message_variant> peek() override;
	size_t availableAmount() const override;

	uint16_t stream() const;
	string label() const;
	string protocol() const;
	Reliability reliability() const;

	bool isOpen(void) const;
	bool isClosed(void) const;
	size_t maxMessageSize() const;

	void shiftStream();

	virtual void open(shared_ptr<SctpTransport> transport);
	virtual void processOpenMessage(message_ptr);

protected:
	const weak_ptr<impl::PeerConnection> mPeerConnection;
	weak_ptr<SctpTransport> mSctpTransport;

	uint16_t mStream;
	string mLabel;
	string mProtocol;
	shared_ptr<Reliability> mReliability;

	mutable std::shared_mutex mMutex;

	Queue<message_ptr> mRecvQueue;

	std::atomic<bool> mIsOpen = false;
	std::atomic<bool> mIsClosed = false;
};

struct NegotiatedDataChannel final : public DataChannel {
	NegotiatedDataChannel(weak_ptr<PeerConnection> pc, uint16_t stream, string label,
	                      string protocol, Reliability reliability);
	NegotiatedDataChannel(weak_ptr<PeerConnection> pc, weak_ptr<SctpTransport> transport,
	                      uint16_t stream);
	~NegotiatedDataChannel();

	void open(impl_ptr<SctpTransport> transport) override;
	void processOpenMessage(message_ptr message) override;
};

} // namespace rtc::impl

#endif
