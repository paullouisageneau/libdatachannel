/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "track.hpp"
#include "internals.hpp"
#include "logcounter.hpp"
#include "peerconnection.hpp"
#include "rtp.hpp"

namespace rtc::impl {

static LogCounter COUNTER_MEDIA_BAD_DIRECTION(plog::warning,
                                              "Number of media packets sent in invalid directions");
static LogCounter COUNTER_QUEUE_FULL(plog::warning,
                                     "Number of media packets dropped due to a full queue");

Track::Track(weak_ptr<PeerConnection> pc, Description::Media desc)
    : mPeerConnection(std::move(pc)), mMediaDescription(std::move(desc)),
      mRecvQueue(RECV_QUEUE_LIMIT, [](const message_ptr &m) { return m->size(); }) {

	// Discard messages by default if track is send only
	if (mMediaDescription.direction() == Description::Direction::SendOnly)
		messageCallback = [](message_variant) {};
}

Track::~Track() {
	PLOG_VERBOSE << "Destroying Track";
	try {
		close();
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}
}

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

void Track::setDescription(Description::Media desc) {
	{
		std::unique_lock lock(mMutex);
		if (desc.mid() != mMediaDescription.mid())
			throw std::logic_error("Media description mid does not match track mid");

		mMediaDescription = std::move(desc);
	}

	if (auto handler = getMediaHandler())
		handler->mediaChain(description());
}

void Track::close() {
	PLOG_VERBOSE << "Closing Track";

#if RTC_ENABLE_MEDIA
	if (!mIsClosed.load())
		sendRtcpByeIfApplicable();
#endif

	if (!mIsClosed.exchange(true))
	{
		triggerClosed();
		setMediaHandler(nullptr);
		resetCallbacks();
	}
}

message_variant Track::trackMessageToVariant(message_ptr message) {
	if (message->type == Message::Control)
		return to_variant(*message); // The same message may be frowarded into multiple Tracks
	else
		return to_variant(std::move(*message));
}

optional<message_variant> Track::receive() {
	if (auto next = mRecvQueue.pop()) {
		return trackMessageToVariant(*next);
	}
	return nullopt;
}

optional<message_variant> Track::peek() {
	if (auto next = mRecvQueue.peek()) {
		return trackMessageToVariant(*next);
	}
	return nullopt;
}

size_t Track::availableAmount() const { return mRecvQueue.amount(); }

bool Track::isOpen() const {
#if RTC_ENABLE_MEDIA
	std::shared_lock lock(mMutex);
	return !mIsClosed && mDtlsSrtpTransport.lock();
#else
	return false;
#endif
}

bool Track::isClosed() const { return mIsClosed; }

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

	if (!mIsClosed)
		triggerOpen();
}

void Track::sendRtcpByeIfApplicable() {
	auto pc = mPeerConnection.lock();
	if (!pc || !pc->config.sendRtcpByeOnTrackClose)
		return;

	shared_ptr<DtlsSrtpTransport> transport;
	Description::Direction dir;
	std::vector<SSRC> ssrcs;
	std::vector<string> cnames;
	{
		std::shared_lock lock(mMutex);

		// Only audio and video tracks carry RTP/RTCP traffic — application (data channel) and
		// other non-media m= sections never have an SSRC stream and must not emit BYE.
		auto mediaType = mMediaDescription.type();
		if (mediaType != "audio" && mediaType != "video")
			return;

		transport = mDtlsSrtpTransport.lock();
		if (!transport)
			return;
		dir = mMediaDescription.direction();
		ssrcs = mMediaDescription.getSSRCs();
		cnames.reserve(ssrcs.size());
		for (auto ssrc : ssrcs)
			cnames.push_back(mMediaDescription.getCNameForSsrc(ssrc).value());
	}

	// RFC 3550 section 6.3.7: a participant which never sent RTP/RTCP MUST NOT send BYE.
	// Only send for tracks we were transmitting on.
	if (dir != Description::Direction::SendOnly && dir != Description::Direction::SendRecv)
		return;

	// One compound RR + SDES + BYE per SSRC
	for (size_t i = 0; i < ssrcs.size(); ++i) {
		SSRC ssrc = ssrcs[i];

		size_t rrSize = RtcpRr::SizeWithReportBlocks(0);
		size_t sdesSize = RtcpSdes::Size({{uint8_t(cnames[i].size())}});
		size_t byeSize = RtcpBye::SizeWithSsrcs(1);

		auto msg = make_message(rrSize + sdesSize + byeSize, Message::Control);

		auto rr = reinterpret_cast<RtcpRr *>(msg->data());
		rr->preparePacket(ssrc, 0);

		auto sdes = reinterpret_cast<RtcpSdes *>(msg->data() + rrSize);
		auto chunk = sdes->getChunk(0);
		chunk->setSSRC(ssrc);
		auto item = chunk->getItem(0);
		item->type = 1; // CNAME
		item->setText(cnames[i]);
		sdes->preparePacket(1);

		auto bye = reinterpret_cast<RtcpBye *>(msg->data() + rrSize + sdesSize);
		bye->preparePacket(1);
		bye->setSsrc(0, ssrc);

		try {
			transport->sendMedia(msg);
		} catch (const std::exception &e) {
			PLOG_WARNING << "Failed to send RTCP BYE for SSRC " << ssrc << ": " << e.what();
		}
	}
}
#endif

void Track::incoming(message_ptr message) {
	if (!message)
		return;

	auto dir = direction();
	if ((dir == Description::Direction::SendOnly || dir == Description::Direction::Inactive) &&
	    message->type != Message::Control) {
		COUNTER_MEDIA_BAD_DIRECTION++;
		return;
	}

	message_vector messages{std::move(message)};
	if (auto handler = getMediaHandler()) {
		try {
			handler->incomingChain(messages, [weak_this = weak_from_this()](message_ptr m) {
				if (auto locked = weak_this.lock()) {
					locked->transportSend(m);
				}
			});
		} catch (const std::exception &e) {
			PLOG_WARNING << "Exception in incoming media handler: " << e.what();
			return;
		}
	}

	for (auto &m : messages) {
		// Tail drop if queue is full
		if (mRecvQueue.full()) {
			COUNTER_QUEUE_FULL++;
			return;
		}

		mRecvQueue.push(m);
		triggerAvailable(mRecvQueue.size());
	}
}

bool Track::outgoing(message_ptr message) {
	if (mIsClosed)
		throw std::runtime_error("Track is closed");

	auto handler = getMediaHandler();

	// If there is no handler, the track expects RTP or RTCP packets
	if (!handler && IsRtcp(*message))
		message->type = Message::Control; // to allow sending RTCP packets irrelevant of direction

	auto dir = direction();
	if ((dir == Description::Direction::RecvOnly || dir == Description::Direction::Inactive) &&
	    message->type != Message::Control) {
		COUNTER_MEDIA_BAD_DIRECTION++;
		return false;
	}

	if (handler) {
		message_vector messages{std::move(message)};
		handler->outgoingChain(messages, [weak_this = weak_from_this()](message_ptr m) {
			if (auto locked = weak_this.lock()) {
				locked->transportSend(m);
			}
		});

		bool ret = false;
		for (auto &m : messages)
			ret = transportSend(std::move(m));

		return ret;

	} else {
		return transportSend(std::move(message));
	}
}

bool Track::transportSend([[maybe_unused]] message_ptr message) {
#if RTC_ENABLE_MEDIA
	shared_ptr<DtlsSrtpTransport> transport;
	{
		std::shared_lock lock(mMutex);
		transport = mDtlsSrtpTransport.lock();
		if (!transport)
			throw std::runtime_error("Track is not open");

		// Set recommended medium-priority DSCP value
		// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
		if (mMediaDescription.type() == "audio")
			message->dscp = 46; // EF: Expedited Forwarding
		else
			message->dscp = 36; // AF42: Assured Forwarding class 4, medium drop probability
	}

	return transport->sendMedia(message);
#else
	throw std::runtime_error("Track is disabled (not compiled with media support)");
#endif
}

void Track::setMediaHandler(shared_ptr<MediaHandler> handler) {
	{
		std::unique_lock lock(mMutex);
		mMediaHandler = handler;
	}

	if (handler)
		handler->mediaChain(description());
}

shared_ptr<MediaHandler> Track::getMediaHandler() {
	std::shared_lock lock(mMutex);
	return mMediaHandler;
}

void Track::flushPendingMessages() {
	if (!mOpenTriggered)
		return;

	while (messageCallback || frameCallback) {
		auto next = mRecvQueue.pop();
		if (!next)
			break;

		auto message = next.value();
		try {
			if (message->frameInfo && frameCallback) {
				frameCallback(std::move(*message), std::move(*message->frameInfo));
			} else if (!message->frameInfo && messageCallback) {
				messageCallback(trackMessageToVariant(message));
			}
		} catch (const std::exception &e) {
			PLOG_WARNING << "Uncaught exception in callback: " << e.what();
		}
	}
}

} // namespace rtc::impl
