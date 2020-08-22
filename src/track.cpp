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

#include "track.hpp"
#include "dtlssrtptransport.hpp"
#include "include.hpp"

namespace rtc {

using std::shared_ptr;
using std::weak_ptr;

Track::Track(string mid, shared_ptr<DtlsSrtpTransport> transport)
    : mMid(std::move(mid)), mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {

	if (transport)
		open(transport);
}

string Track::mid() const { return mMid; }

void Track::close() {
	mIsClosed = true;
	resetCallbacks();
}

bool Track::send(message_variant data) { return outgoing(make_message(std::move(data))); }

bool Track::send(const byte *data, size_t size) {
	return outgoing(std::make_shared<Message>(data, data + size, Message::Binary));
}

std::optional<message_variant> Track::receive() {
	if (!mRecvQueue.empty())
		return to_variant(std::move(**mRecvQueue.pop()));

	return nullopt;
}

bool Track::isOpen(void) const { return !mIsClosed && mDtlsSrtpTransport.lock(); }

bool Track::isClosed(void) const { return mIsClosed; }

size_t Track::maxMessageSize() const {
	return 65535 - 12 - 4; // SRTP/UDP
}

size_t Track::availableAmount() const { return mRecvQueue.amount(); }

void Track::open(shared_ptr<DtlsSrtpTransport> transport) { mDtlsSrtpTransport = transport; }

bool Track::outgoing(message_ptr message) {
	if (mIsClosed)
		throw std::runtime_error("Track is closed");

	auto transport = mDtlsSrtpTransport.lock();
	if (!transport)
		throw std::runtime_error("Track transport is not open");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

	return transport->send(message);
}

void Track::incoming(message_ptr message) {
	if (!message)
		return;

	// Tail drop if queue is full
	if (mRecvQueue.full())
		return;

	mRecvQueue.push(message);
	triggerAvailable(mRecvQueue.size());
}

} // namespace rtc
