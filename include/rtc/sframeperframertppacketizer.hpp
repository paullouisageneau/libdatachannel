/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_PER_FRAME_RTP_PACKETIZER_H
#define RTC_SFRAME_PER_FRAME_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtppacketizer.hpp"
#include "sframe.hpp"

#include <memory>
#include <mutex>

namespace rtc {

/// RTP packetizer for SFrame RFC mode (draft-ietf-avtcore-rtp-sframe).
///
/// Prepends a 1-byte S/E/T descriptor to each RTP payload in place of codec-specific
/// fragmentation, so one packetizer serves both audio and video. It always encrypts: a peer that
/// declines a=sframe has its m-line stopped rather than downgraded, so this packetizer is never
/// installed on a track that sends in the clear.
///
/// The RTP marker ends an SFrame object rather than an RTP timestamp, so a timestamp carrying
/// several objects -- as SVC layers would -- is not expressible through this API. Receivers
/// delimit objects by the E bit, which holds either way.
class RTC_CPP_EXPORT SFramePerFrameRtpPacketizer final : public RtpPacketizer {
public:
	/// Use one SFrameSendKeyProvider per base key, for the life of the session. Packetizers may
	/// share one freely -- the provider hands out one encoder per SSRC, or one for all of them with
	/// per-SSRC derivation off, so they share its counter -- but two providers constructed over the same base key and KID cannot be detected
	/// by this API, and each starts its encoders at ctrStart, so both emit the same nonces.
	///
	/// Throws std::invalid_argument if keyProvider is null.
	/// @param rtpConfig RTP packetization configuration
	/// @param keyProvider Supplies the key to encrypt under, and fixes the session's settings
	/// @param maxFragmentSize Maximum size of one RTP payload, descriptor byte included
	SFramePerFrameRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
	                            shared_ptr<SFrameSendKeyProvider> keyProvider,
	                            size_t maxFragmentSize = DefaultMaxFragmentSize);
	~SFramePerFrameRtpPacketizer();

	void outgoing(message_vector &messages, const message_callback &send) override;


	/// This handler applies SFrame, so an answer for this m-line may assert "a=sframe".
	bool appliesSFrame() const override { return true; }

protected:
	/// Encrypts the frame, then splits it into chunks of at most mMaxFragmentSize bytes, each
	/// prefixed with a 1-byte S/E/T descriptor. Unconditional: a declined m-line is stopped, so
	/// this is never reached for a track sending in the clear.
	std::vector<binary> fragment(binary data) override;

private:
	const size_t mMaxFragmentSize;

	// The encoder owns the send-side counter, so a track must never end up with two: both would
	// start at ctrStart and collide on every frame. call_once serialises the racing first calls to
	// outgoing(). Shared because the provider hands the same encoder to every track when they
	// share a key -- one encoder is one (key, counter) domain.
	std::once_flag mEncoderOnce;
	shared_ptr<impl::SFrameEncoder> mSFrameEncoder;

	// The encoder is built from it on the first frame, once rtpConfig->ssrc is final.
	shared_ptr<SFrameSendKeyProvider> mSFrameKeyProvider;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_SFRAME_PER_FRAME_RTP_PACKETIZER_H */
