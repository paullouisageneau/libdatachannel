/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframeperframeaudiortpdepacketizer.hpp"
#include "rtp.hpp"

#include "impl/internals.hpp"
#include "impl/logcounter.hpp"
#include "impl/sframecodec.hpp"
#include "impl/sframeutility.hpp"

#include <algorithm>

namespace rtc {

static impl::LogCounter COUNTER_SFRAME_PACKETIZED_ORIGIN(
    plog::warning, "Number of SFrame audio packets dropped because the peer applied SFrame "
                   "per RTP packet (descriptor T=1), which is not supported");

SFramePerFrameAudioRtpDepacketizer::SFramePerFrameAudioRtpDepacketizer(
    uint32_t clockRate, shared_ptr<SFrameReceiveKeyProvider> keyProvider)
    : RtpDepacketizer(clockRate),
      mSFrameDecoders(std::make_unique<impl::SFrameDecoderSet>(std::move(keyProvider))) {}

SFramePerFrameAudioRtpDepacketizer::~SFramePerFrameAudioRtpDepacketizer() {}

SFramePerFrameAudioRtpDepacketizer::StreamState &
SFramePerFrameAudioRtpDepacketizer::streamFor(SSRC ssrc) {
	// The stream idle longest goes first; losing its partial only matters if that SSRC returns.
	while (mStreams.size() >= impl::sframe::MaxSFrameStreams &&
	       mStreams.find(ssrc) == mStreams.end()) {
		auto idlest = mStreams.begin();
		for (auto it = mStreams.begin(); it != mStreams.end(); ++it)
			if (it->second.lastArrival < idlest->second.lastArrival)
				idlest = it;
		mStreams.erase(idlest);
	}

	auto &stream = mStreams[ssrc];
	stream.lastArrival = ++mSFrameArrivals;
	return stream;
}

void SFramePerFrameAudioRtpDepacketizer::discardPartial(StreamState &stream, const char *reason) {
	if (stream.partial.empty())
		return;

	PLOG_DEBUG << "Discarding " << stream.partial.size()
	           << " buffered bytes of a partial frame: " << reason;
	stream.partial.clear();
}

bool SFramePerFrameAudioRtpDepacketizer::accumulate(StreamState &stream, const message_ptr &packet,
                                                    size_t hdrSize, size_t payloadEnd,
                                                    message_vector &result) {
	auto pkt = reinterpret_cast<const RtpHeader *>(packet->data());
	const uint8_t descriptor = static_cast<uint8_t>(*(packet->begin() + hdrSize));
	const bool start = (descriptor & impl::SFrameDescriptorS) != 0;
	const bool end = (descriptor & impl::SFrameDescriptorE) != 0;

	// Counted separately: T=1 is a mode we decline, not corruption.
	if (impl::sframe::IsPacketizedOrigin(descriptor)) {
		COUNTER_SFRAME_PACKETIZED_ORIGIN++;
		discardPartial(stream, "peer applied SFrame per RTP packet (descriptor T=1)");
		return false;
	}

	if (start) {
		// A new frame supersedes whatever was in flight; the previous one lost its E.
		discardPartial(stream, "new frame started before the previous one ended");
		stream.partialTimestamp = pkt->timestamp();
		stream.partialPayloadType = pkt->payloadType();
	} else {
		if (stream.partial.empty()) {
			// A continuation with nothing to continue: the S packet was lost or reordered.
			PLOG_DEBUG << "SFrame continuation packet with no frame in progress";
			return false;
		}
		if (pkt->timestamp() != stream.partialTimestamp) {
			discardPartial(stream, "timestamp changed mid-frame");
			return false;
		}
		// Fragments are appended in arrival order, so the sequence number has to be the
		// expected one. A gap means a lost packet and anything else means reordering;
		// joining either way would splice the ciphertext wrong.
		if (pkt->seqNumber() != stream.nextSeqNumber) {
			discardPartial(stream, "fragment out of sequence");
			return false;
		}
	}

	stream.nextSeqNumber = static_cast<uint16_t>(pkt->seqNumber() + 1);

	const size_t payloadSize = payloadEnd - (hdrSize + 1);
	if (stream.partial.size() + payloadSize > MaxPartialSize) {
		discardPartial(stream, "frame exceeds the reassembly limit");
		return false;
	}

	stream.partial.insert(stream.partial.end(), packet->begin() + hdrSize + 1,
	                      packet->begin() + payloadEnd);

	if (!end)
		return true; // more packets to come

	auto frameInfo = createFrameInfo(stream.partialTimestamp, stream.partialPayloadType);
	result.push_back(make_message(std::move(stream.partial), std::move(frameInfo)));
	stream.partial.clear();
	return true;
}

void SFramePerFrameAudioRtpDepacketizer::incoming(message_vector &messages,
                                                  [[maybe_unused]] const message_callback &send) {
	message_vector result;
	result.reserve(messages.size());
	// One SSRC per call, since PeerConnection routes each packet by SSRC.
	bool ssrcValid = false;
	SSRC ssrc = 0;

	// Split by SSRC only with per-SSRC derivation on; see the video depacketizer.
	const bool perSsrcKeys = mSFrameDecoders->usePerSSRCDerivation();
	std::vector<SSRC> owners;
	SSRC firstOwner = 0;
	bool severalOwners = false;
	auto tagAppended = [&](SSRC owner) {
		if (!perSsrcKeys)
			return;
		if (owner && result.size() > owners.size()) {
			if (!firstOwner)
				firstOwner = owner;
			else if (owner != firstOwner)
				severalOwners = true;
		}
		owners.resize(result.size(), owner);
	};

	for (auto &message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			tagAppended(0); // Control never decrypts
			continue;
		}

		// Shared framing check; drops malformed packets. A packet with no payload once padding
		// is removed is part of no frame, but it took a sequence number, so it still has to be
		// accounted for. Either outcome means ParseRtpPayload validated the RTP header, so the
		// SSRC below can be read.
		size_t hdrSize = 0, payloadEnd = 0;
		const bool parsed = impl::sframe::ParseSFramePacket(message, hdrSize, payloadEnd);
		const bool noPayload = !parsed && impl::sframe::HasNoRtpPayload(*message);
		if (!parsed && !noPayload)
			continue;

		auto pkt = reinterpret_cast<const RtpHeader *>(message->data());
		ssrcValid = true;
		ssrc = pkt->ssrc();
		StreamState &stream = streamFor(ssrc);

		if (noPayload) {
			// Without this the next real fragment looks out of sequence and the frame in
			// progress is discarded.
			if (pkt->seqNumber() == stream.nextSeqNumber)
				stream.nextSeqNumber = static_cast<uint16_t>(pkt->seqNumber() + 1);
			continue;
		}

		accumulate(stream, message, hdrSize, payloadEnd, result);
		tagAppended(ssrc);
	}

	messages.swap(result);

	if (!ssrcValid) {
		// No SSRC to derive the per-SSRC key from: drop the media, fail closed. RTCP passes.
		messages.erase(
		    std::remove_if(messages.begin(), messages.end(),
		                   [](const message_ptr &m) { return m->type != Message::Control; }),
		    messages.end());
		return;
	}

	if (severalOwners)
		impl::sframe::DecryptMessages(messages, owners, *mSFrameDecoders);
	else
		impl::sframe::DecryptMessages(messages, ssrc, *mSFrameDecoders);
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
