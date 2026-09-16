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

#if RTC_ENABLE_MEDIA
#include "rtc/sframeperframeaudiortpdepacketizer.hpp"
#include "rtc/sframeperframertppacketizer.hpp"
#include "rtc/sframeperframevideortpdepacketizer.hpp"
#endif

#include <stdexcept>
#include <vector>

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

#if RTC_ENABLE_MEDIA
void Track::enableSFrame(shared_ptr<SFrameReceiveKeyProvider> keyProvider,
                         optional<uint32_t> clockRate) {
	if (!keyProvider)
		throw std::invalid_argument("SFrame key provider is null");

	auto desc = description();

	shared_ptr<MediaHandler> sframeHandler;
	if (desc.type() == "video") {
		sframeHandler =
		    std::make_shared<SFramePerFrameVideoRtpDepacketizer>(std::move(keyProvider));
	} else {
		// RtpDepacketizer holds one clock rate per m-line, so take the first negotiated payload
		// type: SDP lists them in preference order, and it is only the wrong answer on an
		// m-line mixing codecs whose rates differ.
		uint32_t rate = clockRate.value_or(0);
		if (rate == 0) {
			for (int payloadType : desc.payloadTypes()) {
				// A static payload type carries no a=rtpmap (RFC 4566) and rtpMap() throws for it.
				if (!desc.hasPayloadType(payloadType))
					continue;
				const auto *map = desc.rtpMap(payloadType);
				if (map->clockRate > 0) {
					rate = uint32_t(map->clockRate);
					break;
				}
			}
		}
		if (rate == 0)
			throw std::invalid_argument("SFrame: the track declares no RTP clock rate, so one "
			                            "must be passed to useSFrame()");

		sframeHandler =
		    std::make_shared<SFramePerFrameAudioRtpDepacketizer>(rate, std::move(keyProvider));
	}

	// Whatever the application already installed keeps running behind SFrame, since dropping it
	// would take RTX unwrapping and RTCP handling with it, silently. SFrame goes at the head so
	// that it is last on the way in: incoming() runs from the tail of the chain, and RTX has to be
	// unwrapped before the descriptor byte is read.
	//
	// A codec depacketizer is the exception. It occupies the same stage as SFrame on the incoming
	// path -- both consume RTP packets and emit whole frames -- so the two cannot both run, and one
	// left in the chain would strip the RTP headers before SFrame ever saw them. Only depacketizers
	// are skipped: a packetizer is the outgoing stage, and a sendrecv track needs its own alongside
	// this handler.
	std::vector<shared_ptr<MediaHandler>> keep;
	for (auto handler = getMediaHandler(); handler; handler = handler->next()) {
		// Asked by type rather than through a virtual on MediaHandler: which stage a handler
		// occupies is not part of that interface, and adding it would grow the public API.
		if (dynamic_cast<RtpDepacketizer *>(handler.get())) {
			PLOG_INFO << "SFrame is replacing the codec depacketizer on mid=\"" << desc.mid()
			          << "\"";
			continue;
		}
		keep.push_back(handler);
	}

	// Collected before any rewiring, because setNext() overwrites the links being walked. The chain
	// is also assembled before it is installed, so that setMediaHandler() propagates the
	// description through all of it.
	for (size_t i = 0; i + 1 < keep.size(); ++i)
		keep[i]->setNext(keep[i + 1]);
	if (!keep.empty()) {
		keep.back()->setNext(nullptr);
		sframeHandler->addToChain(keep.front());
	}

	setMediaHandler(sframeHandler);

	// Deliberately does not add a=sframe. Asserting it in an answer the offer never carried
	// would both be a protocol error and enable SFrame on this end, so the peer's plain media
	// would all be dropped. Enabling only keeps an offered attribute alive; an offerer adds it
	// to the Description::Media it passes to addTrack().
	if (!desc.hasSFrame()) {
		PLOG_WARNING << "SFrame enabled on mid=\"" << desc.mid()
		             << "\" but a=sframe was not negotiated, so media is NOT protected";
	}
}

bool Track::hasSFrameDepacketizer() {
	for (auto handler = getMediaHandler(); handler; handler = handler->next())
		if (dynamic_cast<SFramePerFrameVideoRtpDepacketizer *>(handler.get()) ||
		    dynamic_cast<SFramePerFrameAudioRtpDepacketizer *>(handler.get()))
			return true;

	return false;
}
#endif

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

#if RTC_ENABLE_MEDIA
	// Refused rather than sent: the description tells the peer, and any relay, that this track is
	// end-to-end protected, so emitting plaintext under it is worse than failing. RTCP is exempt --
	// SFrame protects media only. An application that encrypts outside the library must not
	// advertise a=sframe through this track's description.
	if (message->type != Message::Control &&
	    !mSFrameSendChecked.load(std::memory_order_relaxed)) {
		if (sframeSendsUnprotected())
			throw std::logic_error("Track mid=\"" + mid() +
			                       "\" negotiated a=sframe but no SFrame packetizer is installed, so "
			                       "outgoing media would not be encrypted: add "
			                       "SFramePerFrameRtpPacketizer to the track's media handler chain");

		mSFrameSendChecked.store(true, std::memory_order_relaxed);
	}
#endif

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

#if RTC_ENABLE_MEDIA
// Asked at send time rather than when the chain is built: a chain can be appended to through
// addToChain() without going through setMediaHandler(), and a description can arrive with the
// constructor, so there is no one place this could be recorded from. The first frame is also the
// earliest point the chain is final for both roles -- the deferred model installs the send side only
// once the answer has been read.
//
// Asked by type because appliesSFrame() answers for the whole chain rather than one handler, so a
// codec packetizer ahead of an SFrame depacketizer would read as protecting the send side.
// Only the track's own chain, deliberately -- not the PeerConnection's, unlike the receive-side
// question in processRemoteDescription(). The asymmetry mirrors the library: forwardMedia() runs the
// session-wide chain on incoming media, but outgoing media never touches it (see the TODO there),
// so a packetizer installed session-wide would not encrypt anything and the throw is correct.
bool Track::sframeSendsUnprotected() {
	{
		std::shared_lock lock(mMutex);
		if (!mMediaDescription.hasSFrame())
			return false;
	}

	for (auto handler = getMediaHandler(); handler; handler = handler->next())
		if (dynamic_cast<SFramePerFrameRtpPacketizer *>(handler.get()))
			return false;

	return true;
}
#endif

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
