/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_TRACK_H
#define RTC_TRACK_H

#include "channel.hpp"
#include "common.hpp"
#include "description.hpp"
#include "mediahandler.hpp"

#include <vector>

namespace rtc {

namespace impl {

class Track;

} // namespace impl

class SFrameReceiveKeyProvider;

class RTC_CPP_EXPORT Track final : private CheshireCat<impl::Track>, public Channel {
public:
	Track(impl_ptr<impl::Track> impl);
	~Track() override;

	string mid() const;
	Description::Direction direction() const;
	Description::Media description() const;

	void setDescription(Description::Media description);

	void close(void) override;

	/// Sends media on the track.
	///
	/// Throws std::runtime_error if the track is closed, and std::logic_error if the track's
	/// description carries "a=sframe" but no SFrame packetizer is installed: the description tells
	/// the peer, and any relay, that the media is end-to-end protected, so sending is refused
	/// rather than putting plaintext under that claim. RTCP is unaffected. Checked on the first
	/// frame, because a handler chain is legitimately incomplete while it is being built and the
	/// deferred form installs the send side only once the answer has been applied.
	bool send(message_variant data) override;

	/// @copydoc send(message_variant)
	bool send(const byte *data, size_t size) override;

	bool isOpen(void) const override;
	bool isClosed(void) const override;
	size_t maxMessageSize() const override;

	/// Sends one media frame with its timing information. Throws as send() does, including the
	/// std::logic_error for "a=sframe" with no SFrame packetizer installed.
	void sendFrame(binary data, FrameInfo info);

	/// @copydoc sendFrame(binary, FrameInfo)
	void sendFrame(const byte *data, size_t size, FrameInfo info);
	void onFrame(std::function<void(binary data, FrameInfo info)> callback);

	bool requestKeyframe(SSRC ssrc=0, bool retransmit=false);
	bool requestKeyframe(const std::vector<SSRC>& targetSSRCs, bool retransmit=false);
	bool requestBitrate(unsigned int bitrate);
	bool sendRtcpApp(uint32_t ssrc, const RtcpAppName &name, uint8_t subtype,
	                 const binary &data = binary{});

#if RTC_ENABLE_MEDIA
	/// Receives SFrame (RFC 9605) on this track: installs the per-frame depacketizer for the
	/// track's media kind and keeps "a=sframe" in the local description. Not calling it declines
	/// SFrame, which stops the m-line rather than falling back to plaintext.
	///
	/// Call it from the track callback, and register PeerConnection::onTrack() before applying the
	/// offer -- the answer is generated inside setRemoteDescription(), so a later callback runs
	/// after the m-line has been stopped.
	///
	/// A codec depacketizer already on the track is replaced, since it occupies the same stage. Do
	/// not chain one after this call: it would run ahead of SFrame and nothing detects that.
	///
	/// @param keyProvider Supplies the key and sframe settings.
	/// @param clockRate Audio RTP clock rate.
	/// @throws std::invalid_argument if keyProvider is null, or if clockRate is unset and cannot be
	///         resolved. From the track callback the throw is caught and logged and the m-line is
	///         stopped, so watch for onClosed() rather than expecting it at the call.
	void useSFrame(shared_ptr<SFrameReceiveKeyProvider> keyProvider,
	               optional<uint32_t> clockRate = nullopt);
#endif

	void setMediaHandler(shared_ptr<MediaHandler> handler);
	void chainMediaHandler(shared_ptr<MediaHandler> handler);
	shared_ptr<MediaHandler> getMediaHandler();

	// Deprecated, use setMediaHandler() and getMediaHandler()
	inline void setRtcpHandler(shared_ptr<MediaHandler> handler) { setMediaHandler(handler); }
	inline shared_ptr<MediaHandler> getRtcpHandler() { return getMediaHandler(); }

private:
	using CheshireCat<impl::Track>::impl;
};

} // namespace rtc

#endif
