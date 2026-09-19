/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_RTP_PACKETIZER_H
#define RTC_SFRAME_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtppacketizer.hpp"
#include "sframe.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace rtc {

/// RTP packetizer for SFrame RFC mode (draft-ietf-avtcore-rtp-sframe).
///
/// Fragments the frame into MTU-sized chunks and prepends a 1-byte S/E/T descriptor to each
/// RTP payload, in place of codec-specific fragmentation: the frame is an opaque blob, so
/// one packetizer serves both audio and video.
///
/// The per-SSRC key and encoder are built on the first outgoing() call, once
/// rtpConfig->ssrc is final. A peer declining a=sframe disables encryption for the track --
/// see media() -- so check Description::Media::hasSFrame() to know whether media is
/// protected.
class RTC_CPP_EXPORT SFrameRtpPacketizer final : public RtpPacketizer {
public:
	/// Throws std::invalid_argument for a base key below MinBaseKeySize() or an
	/// unsupported mode.
	/// @param rtpConfig RTP packetization configuration
	/// @param sFrameConfig Key material and ratchet period for the send side
	/// @param mode Object granularity; only SFrameMode::PerFrame is implemented
	/// @param maxFragmentSize Maximum size of one RTP payload, descriptor byte included
	SFrameRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig, SFrameConfig sFrameConfig,
	                    SFrameMode mode, size_t maxFragmentSize = DefaultMaxFragmentSize);

	/// Shared keying: no per-SSRC derivation, so every track sends under one key and must
	/// therefore share one counter. Passing the encoder in rather than a config is what
	/// makes that structural -- two tracks given their own encoder off the same key would
	/// both start at ctrStart and reuse every nonce, which on the GCM suites leaks the
	/// GHASH key. The receiving provider must answer false to usePerSSRCDerivation().
	/// @param rtpConfig RTP packetization configuration
	/// @param encoder Encoder shared with every other track on this key
	/// @param mode Object granularity; only SFrameMode::PerFrame is implemented
	/// @param maxFragmentSize Maximum size of one RTP payload, descriptor byte included
	SFrameRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
	                    shared_ptr<SFrameEncoder> encoder, SFrameMode mode,
	                    size_t maxFragmentSize = DefaultMaxFragmentSize);
	~SFrameRtpPacketizer();

	void outgoing(message_vector &messages, const message_callback &send) override;

	/// Enables or disables SFrame for this m-line from the negotiated media description.
	void media(const Description::Media &desc) override;

	bool handlesSFrame() const override { return true; }

protected:
	/// Encrypts the frame and applies SFrame RFC framing: chunks of at most mMaxFragmentSize
	/// bytes, each prefixed with a 1-byte S/E/T descriptor (T=0 raw/per-frame origin), in
	/// place of codec-specific fragmentation. With SFrame declined the frame is passed to the
	/// base class whole, neither encrypted nor fragmented.
	std::vector<binary> fragment(binary data) override;

private:
	const SFrameConfig mSFrameConfig;
	const size_t mMaxFragmentSize;

	// Cleared when a negotiated description carries no a=sframe: encryption and the
	// descriptor both stop, or the peer could not parse what we send. Starts set so a
	// packetizer built directly, without SDP, still protects frames.
	std::atomic<bool> mSFrameNegotiated{true};

	// The encoder owns the send-side counter, so exactly one must ever be created: two
	// encoders built from the same config would both start at ctrStart and collide on
	// every frame. call_once serialises the racing first calls to outgoing().
	// Held by shared_ptr because shared keying gives the same encoder to every track: one
	// encoder is one (key, counter) domain.
	std::once_flag mEncoderOnce;
	shared_ptr<SFrameEncoder> mSFrameEncoder;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_SFRAME_RTP_PACKETIZER_H */
