/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframeperframertppacketizer.hpp"

#include "impl/internals.hpp"
#include "impl/sframecodec.hpp"
#include "impl/sframeutility.hpp"

#include <algorithm>

namespace rtc {

SFramePerFrameRtpPacketizer::SFramePerFrameRtpPacketizer(
    shared_ptr<RtpPacketizationConfig> rtpConfig, shared_ptr<SFrameSendKeyProvider> keyProvider,
    size_t maxFragmentSize)
    : RtpPacketizer(rtpConfig), mMaxFragmentSize(maxFragmentSize),
      mSFrameKeyProvider(std::move(keyProvider)) {
	if (!mSFrameKeyProvider)
		throw std::invalid_argument("SFrame send key provider is null");
}

SFramePerFrameRtpPacketizer::~SFramePerFrameRtpPacketizer() {}

std::vector<binary> SFramePerFrameRtpPacketizer::fragment(binary data) {
	// Unconditional: SFrame cannot be declined part way through a session. An answer that drops
	// "a=sframe" closes the track rather than downgrading it, so there is no state in which this
	// packetizer is installed and expected to emit anything but ciphertext.
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
		uint8_t descriptor = 0; // T=0 (raw origin)
		if (offset == 0)
			descriptor |= impl::SFrameDescriptorS; // S=1 (start)
		if (offset + len >= data.size())
			descriptor |= impl::SFrameDescriptorE; // E=1 (end)
		pkt.push_back(static_cast<std::byte>(descriptor));
		pkt.insert(pkt.end(), data.begin() + offset, data.begin() + offset + len);
		payloads.push_back(std::move(pkt));
		offset += len;
	}

	if (payloads.empty()) {
		// Defensive: encodeFrame() always emits at least a header and an AEAD tag, so the
		// ciphertext is never empty and this cannot be reached. Every fragment carries a descriptor
		// regardless.
		binary pkt;
		pkt.push_back(static_cast<std::byte>(impl::SFrameDescriptorS | impl::SFrameDescriptorE));
		payloads.push_back(std::move(pkt));
	}
	return payloads;
}

void SFramePerFrameRtpPacketizer::outgoing(message_vector &messages, const message_callback &send) {
	// Build the encoder on the first frame, once rtpConfig->ssrc is final. It costs one key
	// derivation and means a later renegotiation that enables SFrame cannot find it missing.
	{
		std::call_once(mEncoderOnce, [this] {
			// The encoder applies the per-SSRC derivation of draft-ietf-avtcore-rtp-sframe
			// Section 7 itself, to every generation the provider supplies rather than only the
			// first, so it is given the SSRC and the raw provider. With that derivation off it
			// is the provider's own encoder, shared with every other track on the key.
			mSFrameEncoder = impl::SFrameEncoder::ForTrack(mSFrameKeyProvider, rtpConfig->ssrc);
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
