/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframevideortpdepacketizer.hpp"

#include "impl/internals.hpp"
#include "impl/sframeutility.hpp"
#include "rtp.hpp"

#include <algorithm>

namespace rtc {

SFrameVideoRtpDepacketizer::SFrameVideoRtpDepacketizer(
    std::shared_ptr<SFrameKeyProvider> keyProvider)
    : mKeyProvider(std::move(keyProvider)) {
	if (!mKeyProvider)
		throw std::invalid_argument("SFrame key provider is null");

	enableSFrame = true;
}

SFrameVideoRtpDepacketizer::~SFrameVideoRtpDepacketizer() {

}

void SFrameVideoRtpDepacketizer::media(const Description::Media &desc) {
	// Without a=sframe the peer sends plain codec payloads, so the descriptor byte would
	// be read out of the codec's own first byte and every packet dropped.
	const bool negotiated = desc.hasSFrame();
	const bool was = enableSFrame.exchange(negotiated, std::memory_order_relaxed);

	// Losing SFrame is legal but it is a downgrade to unprotected media, and an SDP relay
	// can force it, so say so rather than leave it to be noticed via hasSFrame().
	if (was && !negotiated) {
		PLOG_WARNING << "SFrame declined for mid=" << desc.mid()
		             << ", incoming media on this track is NOT end-to-end protected";
	}

	VideoRtpDepacketizer::media(desc);
}

void SFrameVideoRtpDepacketizer::incoming(message_vector &messages, const message_callback &send) {
	// Capture the SSRC before the base class reassembly strips it: the per-SSRC derivation
	// of draft-ietf-avtcore-rtp-sframe section 7 needs it and FrameInfo does not carry it.
	// One SSRC per call, since PeerConnection routes each packet by SSRC.
	bool ssrcValid = false;
	uint32_t ssrc = 0;

	for (auto &message : messages) {
		if (message->type == Message::Control)
			continue;

		if (message->size() < sizeof(RtpHeader))
			continue;

		auto pkt = reinterpret_cast<const RtpHeader *>(message->data());
		ssrcValid = true;
		ssrc = pkt->ssrc();
	}

	// Let the base class do SFrame RFC assembly (strip S/E/T, reassemble blob).
	VideoRtpDepacketizer::incoming(messages, send);

	// The value the base class just used, not a fresh read: a renegotiation in between would
	// otherwise hand the assembled ciphertext to the application undecrypted.
	if (!sframeBatch)
		return; // SFrame declined for this m-line: nothing to decrypt

	if (!ssrcValid) {
		// No SSRC to derive the per-SSRC key from: drop the media, fail closed. RTCP passes.
		messages.erase(std::remove_if(messages.begin(), messages.end(),
		                              [](const message_ptr &m) {
			                              return m->type != Message::Control;
		                              }),
		               messages.end());
		return;
	}

	impl::SFrameUtility::DecryptMessages(messages, mKeyProvider, ssrc, mSFrameDecoder);
}

message_ptr SFrameVideoRtpDepacketizer::reassemble(message_buffer &buffer) {
	// Reached only when the peer declined a=sframe, so the payloads are plain codec bytes.
	// Concatenate them in sequence order, the same way the audio depacketizer hands its
	// payloads up unchanged. This class knows no codec, so a gap cannot be bridged and the
	// frame is dropped rather than spliced together wrong.
	if (buffer.empty())
		return nullptr;

	auto first = *buffer.begin();
	auto firstHeader = reinterpret_cast<const RtpHeader *>(first->data());
	const uint8_t payloadType = firstHeader->payloadType();
	const uint32_t timestamp = firstHeader->timestamp();
	uint16_t nextSeqNumber = firstHeader->seqNumber();

	binary frame;
	for (const auto &packet : buffer) {
		auto header = reinterpret_cast<const RtpHeader *>(packet->data());
		if (header->seqNumber() != nextSeqNumber)
			return nullptr; // packet lost or reordered
		nextSeqNumber = uint16_t(header->seqNumber() + 1);

		// Bounds checks the extension length and the padding count before either is used.
		size_t headerSize = 0, payloadEnd = 0;
		if (!impl::SFrameUtility::ParseRtpPayload(*packet, headerSize, payloadEnd))
			return nullptr;

		frame.insert(frame.end(), packet->begin() + headerSize, packet->begin() + payloadEnd);
	}

	if (frame.empty())
		return nullptr;

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
