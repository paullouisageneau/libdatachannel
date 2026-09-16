/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframeaudiortpdepacketizer.hpp"

#include "impl/internals.hpp"
#include "impl/sframeutility.hpp"
#include "impl/logcounter.hpp"
#include "rtp.hpp"

#include <algorithm>

namespace rtc {

static impl::LogCounter COUNTER_SFRAME_PACKETIZED_ORIGIN(
    plog::warning, "Number of SFrame audio packets dropped because the peer applied SFrame "
                   "per RTP packet (descriptor T=1), which is not supported");

SFrameAudioRtpDepacketizer::SFrameAudioRtpDepacketizer(uint32_t clockRate,
                                                       std::shared_ptr<SFrameKeyProvider> keyProvider)
    : RtpDepacketizer(clockRate), mKeyProvider(std::move(keyProvider)) {
	if (!mKeyProvider)
		throw std::invalid_argument("SFrame key provider is null");
}

SFrameAudioRtpDepacketizer::~SFrameAudioRtpDepacketizer() {

}

void SFrameAudioRtpDepacketizer::media(const Description::Media &desc) {
	// Without a=sframe the peer sends plain codec payloads, so the descriptor byte would
	// be read out of the codec's own first byte and every packet dropped.
	const bool negotiated = desc.hasSFrame();
	const bool was = mSFrameNegotiated.exchange(negotiated, std::memory_order_relaxed);

	// Losing SFrame is legal but it is a downgrade to unprotected media, and an SDP relay
	// can force it, so say so rather than leave it to be noticed via hasSFrame().
	if (was && !negotiated) {
		PLOG_WARNING << "SFrame declined for mid=" << desc.mid()
		             << ", incoming media on this track is NOT end-to-end protected";
	}

	RtpDepacketizer::media(desc);
}

void SFrameAudioRtpDepacketizer::discardPartial(const char *reason) {
	if (mPartial.empty())
		return;

	PLOG_DEBUG << "[SFRAME-AUDIO] discarding " << mPartial.size()
	           << " buffered bytes of a partial frame: " << reason;
	mPartial.clear();
}

bool SFrameAudioRtpDepacketizer::accumulate(const message_ptr &packet, size_t hdrSize,
                                            size_t payloadEnd, message_vector &result) {
	auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
	const uint8_t descriptor = static_cast<uint8_t>(*(packet->begin() + hdrSize));
	const bool start = (descriptor & SFRAME_DESCRIPTOR_S) != 0;
	const bool end = (descriptor & SFRAME_DESCRIPTOR_E) != 0;

	// Counted separately: T=1 is a mode we decline, not corruption.
	if (impl::SFrameUtility::IsPacketizedOrigin(descriptor)) {
		COUNTER_SFRAME_PACKETIZED_ORIGIN++;
		discardPartial("peer applied SFrame per RTP packet (descriptor T=1)");
		return false;
	}

	// Everything outside S and E must be zero: no reserved bits set.
	if ((descriptor & ~(SFRAME_DESCRIPTOR_S | SFRAME_DESCRIPTOR_E)) != 0) {
		discardPartial("reserved descriptor bits set");
		return false;
	}

	if (start) {
		// A new frame supersedes whatever was in flight; the previous one lost its E.
		discardPartial("new frame started before the previous one ended");
		mPartialTimestamp = pkt->timestamp();
		mPartialPayloadType = pkt->payloadType();
	} else {
		if (mPartial.empty()) {
			// A continuation with nothing to continue: the S packet was lost or reordered.
			PLOG_DEBUG << "[SFRAME-AUDIO] continuation packet with no frame in progress";
			return false;
		}
		if (pkt->timestamp() != mPartialTimestamp) {
			discardPartial("timestamp changed mid-frame");
			return false;
		}
		// Fragments are appended in arrival order, so the sequence number has to be the
		// expected one. A gap means a lost packet and anything else means reordering;
		// joining either way would splice the ciphertext wrong.
		if (pkt->seqNumber() != mNextSeqNumber) {
			discardPartial("fragment out of sequence");
			return false;
		}
	}

	mNextSeqNumber = static_cast<uint16_t>(pkt->seqNumber() + 1);

	const size_t payloadSize = payloadEnd - (hdrSize + 1);
	if (mPartial.size() + payloadSize > MaxPartialSize) {
		discardPartial("frame exceeds the reassembly limit");
		return false;
	}

	mPartial.insert(mPartial.end(), packet->begin() + hdrSize + 1, packet->begin() + payloadEnd);

	if (!end)
		return true; // more packets to come

	auto frameInfo = createFrameInfo(mPartialTimestamp, mPartialPayloadType);
	result.push_back(make_message(std::move(mPartial), std::move(frameInfo)));
	mPartial.clear();
	return true;
}

void SFrameAudioRtpDepacketizer::incoming(message_vector &messages, const message_callback &send) {
	if (!mSFrameNegotiated.load(std::memory_order_relaxed)) {
		// SFrame declined for this m-line: hand the payloads up unchanged.
		RtpDepacketizer::incoming(messages, send);
		return;
	}

	message_vector result;
	result.reserve(messages.size());
	// One SSRC per call, since PeerConnection routes each packet by SSRC.
	bool ssrcValid = false;
	uint32_t ssrc = 0;

	for (auto &message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			continue;
		}

		// Shared framing check; drops malformed packets.
		size_t hdrSize = 0, payloadEnd = 0;
		if (!impl::SFrameUtility::ParseSFramePacket(message, hdrSize, payloadEnd))
			continue;

		auto pkt = reinterpret_cast<const RtpHeader *>(message->data());
		ssrcValid = true;
		ssrc = pkt->ssrc();

		accumulate(message, hdrSize, payloadEnd, result);
	}

	messages.swap(result);

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

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
