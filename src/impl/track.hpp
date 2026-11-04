/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_TRACK_H
#define RTC_IMPL_TRACK_H

#include "channel.hpp"
#include "common.hpp"
#include "description.hpp"
#include "mediahandler.hpp"
#include "queue.hpp"
#include "rtc/sframe.hpp"

#if RTC_ENABLE_MEDIA
#include "dtlssrtptransport.hpp"
#endif

#include <atomic>
#include <shared_mutex>

namespace rtc::impl {

struct PeerConnection;

class Track final : public std::enable_shared_from_this<Track>, public Channel {
public:
	Track(weak_ptr<PeerConnection> pc, Description::Media desc);
	~Track();

	void close();
	void incoming(message_ptr message);
	bool outgoing(message_ptr message);

	optional<message_variant> receive() override;
	optional<message_variant> peek() override;
	size_t availableAmount() const override;
	void flushPendingMessages() override;
	message_variant trackMessageToVariant(message_ptr message);

	void sendFrame(binary data, const FrameInfo &frame);

	bool isOpen() const;
	bool isClosed() const;
	size_t maxMessageSize() const;

	string mid() const;
	Description::Direction direction() const;
	Description::Media description() const;
	void setDescription(Description::Media desc);

#if RTC_ENABLE_MEDIA
	// Installs the per-frame depacketizer for this track's media kind at the head of the chain.
	// Sets no flag and does not touch the description: what keeps a=sframe in an answer is the
	// installed handler reporting appliesSFrame(). Shared by Track::useSFrame() and the
	// session-wide provider PeerConnection applies to each incoming track that negotiated a=sframe.
	void enableSFrame(shared_ptr<SFrameReceiveKeyProvider> keyProvider,
	                  optional<uint32_t> clockRate);

	// Whether the chain already decrypts incoming media, i.e. holds one of the depacketizers
	// enableSFrame() installs. Public because PeerConnection asks it while negotiating.
	//
	// This is the receive-direction question. Its send-direction counterpart is the private
	// sframeSendsUnprotected(), and MediaHandler::appliesSFrame() is a third, chain-wide question --
	// "may an answer assert a=sframe" -- which is true of a send-side packetizer as well and so
	// cannot answer either of the directional ones. Keeping the three apart is the point: asking
	// appliesSFrame() here let a packetizer suppress the receive-side install, and the answer
	// negotiated a=sframe with nothing to decrypt it.
	//
	// Asked by type, as the chain walk in enableSFrame() is, because which stage a handler occupies
	// is not part of the MediaHandler interface. A future per-packet SFrame handler, or an
	// application's own, would not be recognised -- at which point splitting appliesSFrame() into
	// directional virtuals becomes the right answer.
	bool hasSFrameDepacketizer();
#endif

	shared_ptr<MediaHandler> getMediaHandler();
	void setMediaHandler(shared_ptr<MediaHandler> handler);

#if RTC_ENABLE_MEDIA
	void open(shared_ptr<DtlsSrtpTransport> transport);
#endif

	bool transportSend(message_ptr message);

	synchronized_callback<binary, FrameInfo> frameCallback;

private:
	const weak_ptr<PeerConnection> mPeerConnection;
#if RTC_ENABLE_MEDIA
	weak_ptr<DtlsSrtpTransport> mDtlsSrtpTransport;
#endif

	Description::Media mMediaDescription;
	shared_ptr<MediaHandler> mMediaHandler;

	mutable std::shared_mutex mMutex;

	std::atomic<bool> mIsClosed = false;

#if RTC_ENABLE_MEDIA
	// True when the track advertises a=sframe but nothing in the chain encrypts what it sends.
	bool sframeSendsUnprotected();

	// Latched once the send chain has been seen to be correct, so the check runs on the first frame
	// and every frame after it costs one relaxed load.
	std::atomic<bool> mSFrameSendChecked = false;
#endif

	Queue<message_ptr> mRecvQueue;

};

} // namespace rtc::impl

#endif
