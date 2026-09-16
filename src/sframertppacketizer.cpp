/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframertppacketizer.hpp"

#include "impl/internals.hpp"
#include "impl/sframeutility.hpp"

#include <algorithm>

namespace rtc {

SFrameRtpPacketizer::SFrameRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
                                         SFrameConfig sFrameConfig, SFrameMode mode,
                                         size_t maxFragmentSize)
    : RtpPacketizer(rtpConfig), mSFrameConfig(std::move(sFrameConfig)),
      mMaxFragmentSize(maxFragmentSize) {
	if (mode != SFrameMode::PerFrame)
		throw std::invalid_argument("Unsupported SFrame mode");

	// Validate now, not on the first frame: the encoder is built lazily in outgoing().
	impl::SFrameUtility::ValidateBaseKey(mSFrameConfig.keyDetails.baseKey);

	// Not negotiated in SDP, so there is nothing to infer it from and no safe default: the
	// two ends are configured with the same answer or nothing authenticates.
	if (!mSFrameConfig.perSsrcDerivation.has_value())
		throw std::invalid_argument(
		    "SFrameConfig::perSsrcDerivation must be set: it has to match the receiving "
		    "provider's usePerSSRCDerivation()");

	// Checked here too, not just in the lazily built SFrameEncoder, so a key marked for
	// the wrong direction is reported to the caller rather than from the media path.
	if (mSFrameConfig.keyDetails.use != SFrameKeyUse::Encrypt)
		throw std::invalid_argument(
		    "SFrame base key must be marked SFrameKeyUse::Encrypt to send with it");
}

SFrameRtpPacketizer::SFrameRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
                                         shared_ptr<SFrameEncoder> encoder, SFrameMode mode,
                                         size_t maxFragmentSize)
    : RtpPacketizer(rtpConfig), mMaxFragmentSize(maxFragmentSize),
      mSFrameEncoder(std::move(encoder)) {
	if (mode != SFrameMode::PerFrame)
		throw std::invalid_argument("Unsupported SFrame mode");

	if (!mSFrameEncoder)
		throw std::invalid_argument("SFrame encoder is null");

	// Already constructed, so the lazy path must not build a second one: that would give
	// this track its own counter off the shared key.
	std::call_once(mEncoderOnce, [] {});
}

SFrameRtpPacketizer::~SFrameRtpPacketizer() {}

void SFrameRtpPacketizer::media(const Description::Media &desc) {
	const bool negotiated = desc.hasSFrame();
	const bool was = mSFrameNegotiated.exchange(negotiated, std::memory_order_relaxed);

	// An SDP relay can force this, and it is the whole protection of the track, so warn
	// rather than leave the application to poll hasSFrame().
	if (was && !negotiated) {
		PLOG_WARNING << "SFrame declined for mid=" << desc.mid()
		             << ", outgoing media on this track is NOT end-to-end protected";
	}

	RtpPacketizer::media(desc);
}

std::vector<binary> SFrameRtpPacketizer::fragment(binary data) {
	// Read once, here rather than in outgoing(): encryption and framing must agree about
	// whether SFrame is on, and a renegotiation on another thread would otherwise be free to
	// flip the flag between the two decisions and emit ciphertext with no descriptor.
	const bool negotiated = mSFrameNegotiated.load(std::memory_order_relaxed);

	// Without SFrame the descriptor byte has no meaning to the peer, and this class knows no
	// codec to fragment by, so hand the frame to the base class whole. A frame over the MTU
	// then relies on IP fragmentation: a declined track wants its codec's own packetizer.
	if (!negotiated)
		return RtpPacketizer::fragment(std::move(data));

	data = *mSFrameEncoder->encodeFrame(make_message(std::move(data), Message::Binary), {});

	// RFC draft-ietf-avtcore-rtp-sframe Section 5.1.1 per-frame framing. The descriptor is
	// part of the payload, so the chunk itself has to be one byte smaller than the limit.
	const size_t chunkSize = mMaxFragmentSize > 1 ? mMaxFragmentSize - 1 : 1;

	std::vector<binary> payloads;
	size_t offset = 0;
	while (offset < data.size()) {
		const size_t len = std::min(chunkSize, data.size() - offset);
		binary pkt;
		pkt.reserve(1 + len);
		uint8_t descriptor = 0;                                             // T=0 (raw origin)
		if (offset == 0)                 descriptor |= SFRAME_DESCRIPTOR_S; // S=1 (start)
		if (offset + len >= data.size()) descriptor |= SFRAME_DESCRIPTOR_E; // E=1 (end)
		pkt.push_back(static_cast<std::byte>(descriptor));
		pkt.insert(pkt.end(), data.begin() + offset, data.begin() + offset + len);
		payloads.push_back(std::move(pkt));
		offset += len;
	}

	if (payloads.empty()) {
		// Every fragment carries a descriptor, including the only one of an empty frame.
		binary pkt;
		pkt.push_back(static_cast<std::byte>(SFRAME_DESCRIPTOR_S | SFRAME_DESCRIPTOR_E));
		payloads.push_back(std::move(pkt));
	}
	return payloads;
}

void SFrameRtpPacketizer::outgoing(message_vector &messages, const message_callback &send) {
	// Build the encoder on the first frame, once rtpConfig->ssrc is final. It costs one key
	// derivation and means a later renegotiation that enables SFrame cannot find it missing.
	{
		std::call_once(mEncoderOnce, [this] {
			auto config = mSFrameConfig;
			// draft-ietf-avtcore-rtp-sframe Section 7, when the application asked for it:
			// one key per SSRC, so each sender has its own key stream.
			if (config.perSsrcDerivation.value_or(false))
				config.keyDetails.baseKey = impl::SFrameUtility::DeriveSSRCKey(
				    rtpConfig->ssrc, config.keyDetails.baseKey, config.keyDetails.cipherSuiteId);
			mSFrameEncoder = std::make_unique<SFrameEncoder>(config);
		});
	}

	// RTCP is not SFrame encoded. It also skips the base class, which would otherwise
	// RTP-wrap it a second time.
	message_vector control;
	message_vector media;
	media.reserve(messages.size());
	for (auto &msg : messages) {
		if (msg->type == Message::Control)
			control.push_back(std::move(msg));
		else
			media.push_back(std::move(msg));
	}
	messages.swap(media);

	// Base class handles RTP packetisation, calling fragment() to encrypt and frame.
	RtpPacketizer::outgoing(messages, send);

	messages.insert(messages.end(), std::make_move_iterator(control.begin()),
	                std::make_move_iterator(control.end()));
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
