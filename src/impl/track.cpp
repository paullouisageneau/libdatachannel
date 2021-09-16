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
#include "internals.hpp"
#include "logcounter.hpp"
#include "peerconnection.hpp"

namespace rtc::impl {

static LogCounter COUNTER_MEDIA_BAD_DIRECTION(plog::warning,
                                              "Number of media packets sent in invalid directions");
static LogCounter COUNTER_QUEUE_FULL(plog::warning,
                                     "Number of media packets dropped due to a full queue");

Track::Track(weak_ptr<PeerConnection> pc, Description::Media description)
    : mPeerConnection(pc), mMediaDescription(std::move(description)),
      mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {}

string Track::mid() const {
	std::shared_lock lock(mMutex);
	return mMediaDescription.mid();
}

Description::Direction Track::direction() const {
	std::shared_lock lock(mMutex);
	return mMediaDescription.direction();
}

Description::Media Track::description() const {
	std::shared_lock lock(mMutex);
	return mMediaDescription;
}

void Track::setDescription(Description::Media description) {
	std::unique_lock lock(mMutex);
	if (description.mid() != mMediaDescription.mid())
		throw std::logic_error("Media description mid does not match track mid");

	mMediaDescription = std::move(description);
}

void Track::close() {
	mIsClosed = true;

	setMediaHandler(nullptr);
	resetCallbacks();
}

optional<message_variant> Track::receive() {
	if (auto next = mRecvQueue.tryPop())
		return to_variant(std::move(**next));

	return nullopt;
}

optional<message_variant> Track::peek() {
	if (auto next = mRecvQueue.peek())
		return to_variant(std::move(**next));

	return nullopt;
}

size_t Track::availableAmount() const { return mRecvQueue.amount(); }

bool Track::isOpen(void) const {
#if RTC_ENABLE_MEDIA
	std::shared_lock lock(mMutex);
	return !mIsClosed && mDtlsSrtpTransport.lock();
#else
	return !mIsClosed;
#endif
}

bool Track::isClosed(void) const { return mIsClosed; }

size_t Track::maxMessageSize() const {
	optional<size_t> mtu;
	if (auto pc = mPeerConnection.lock())
		mtu = pc->config.mtu;

	return mtu.value_or(DEFAULT_MTU) - 12 - 8 - 40; // SRTP/UDP/IPv6
}

#if RTC_ENABLE_MEDIA
void Track::open(shared_ptr<DtlsSrtpTransport> transport) {
	{
		std::lock_guard lock(mMutex);
		mDtlsSrtpTransport = transport;
	}

	triggerOpen();
}
#endif

void Track::incoming(message_ptr message) {
	if (!message)
		return;

	// TODO
	auto dir = direction();
	if ((dir == Description::Direction::SendOnly || dir == Description::Direction::Inactive) &&
	    message->type != Message::Control) {
		COUNTER_MEDIA_BAD_DIRECTION++;
		return;
	}

	if (auto handler = getMediaHandler()) {
		message = handler->incoming(message);
		if (!message)
			return;
	}

	// Tail drop if queue is full
	if (mRecvQueue.full()) {
		COUNTER_QUEUE_FULL++;
		return;
	}

	mRecvQueue.push(message);
	triggerAvailable(mRecvQueue.size());
}

bool Track::outgoing(message_ptr message) {
	if (mIsClosed)
		throw std::runtime_error("Track is closed");

	auto dir = direction();
	if ((dir == Description::Direction::RecvOnly || dir == Description::Direction::Inactive)) {
		COUNTER_MEDIA_BAD_DIRECTION++;
		return false;
	}

	if (auto handler = getMediaHandler()) {
		message = handler->outgoing(message);
		if (!message)
			return false;
	}

	return transportSend(message);
}

bool Track::transportSend([[maybe_unused]] message_ptr message) {
#if RTC_ENABLE_MEDIA
	shared_ptr<DtlsSrtpTransport> transport;
	{
		std::shared_lock lock(mMutex);
		transport = mDtlsSrtpTransport.lock();
		if (!transport)
			throw std::runtime_error("Track is closed");

		// Set recommended medium-priority DSCP value
		// See https://datatracker.ietf.org/doc/html/rfc8837#section-5
		if (mMediaDescription.type() == "audio")
			message->dscp = 46; // EF: Expedited Forwarding
		else
			message->dscp = 36; // AF42: Assured Forwarding class 4, medium drop probability
	}

	return transport->sendMedia(message);
#else
	PLOG_WARNING << "Ignoring track send (not compiled with media support)";
	return false;
#endif
}

void Track::setMediaHandler(shared_ptr<MediaHandler> handler) {
	{
		std::unique_lock lock(mMutex);
		if (mMediaHandler)
			mMediaHandler->onOutgoing(nullptr);

		mMediaHandler = handler;
	}

	if (handler)
		handler->onOutgoing(std::bind(&Track::transportSend, this, std::placeholders::_1));
}

shared_ptr<MediaHandler> Track::getMediaHandler() {
	std::shared_lock lock(mMutex);
	return mMediaHandler;
}

} // namespace rtc::impl
