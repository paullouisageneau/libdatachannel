/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_PER_FRAME_AUDIO_RTP_DEPACKETIZER_H
#define RTC_SFRAME_PER_FRAME_AUDIO_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtpdepacketizer.hpp"
#include "sframe.hpp"

#include <map>
#include <memory>

namespace rtc {

/// RTP depacketizer for SFrame RFC mode on audio tracks (draft-ietf-avtcore-rtp-sframe).
///
/// Strips the 1-byte S/E/T descriptor and decrypts. Audio fragments on the same MTU boundary as
/// video, so frames are reassembled from S...E. Fragments are appended in arrival order: one out
/// of sequence or after a gap discards the partial frame rather than being spliced in.
class RTC_CPP_EXPORT SFramePerFrameAudioRtpDepacketizer final : public RtpDepacketizer {
public:
	/// Throws std::invalid_argument if keyProvider is null.
	/// @param clockRate RTP clock rate of the audio track
	/// @param keyProvider Supplies the key for each generation, and fixes the session's
	///                    cipher suite, KID layout and per-SSRC derivation setting
	SFramePerFrameAudioRtpDepacketizer(uint32_t clockRate,
	                                   shared_ptr<SFrameReceiveKeyProvider> keyProvider);
	~SFramePerFrameAudioRtpDepacketizer();

	void incoming(message_vector &messages, const message_callback &send) override;


	/// This handler applies SFrame, so an answer for this m-line may assert "a=sframe".
	bool appliesSFrame() const override { return true; }

private:
	// Every SSRC on an m-line resolves to one track, so simulcast, RTX and FEC all arrive here.
	// Sequence spaces are per stream, so a frame under construction is too: shared, two interleaved
	// streams would each look out of sequence to the other and discard the partial every packet.
	struct StreamState {
		// Frame under construction. Empty when no S has been seen since the last E.
		binary partial;
		uint32_t partialTimestamp = 0;
		uint8_t partialPayloadType = 0;

		// Sequence number the next fragment must carry. A gap means a packet was lost and a
		// repeat or a lower value means reordering; either way the fragments cannot be joined.
		uint16_t nextSeqNumber = 0;

		uint64_t lastArrival = 0; // to evict the stream idle longest
	};
	std::map<SSRC, StreamState> mStreams;
	uint64_t mSFrameArrivals = 0;

	StreamState &streamFor(SSRC ssrc);

	// Appends one packet's payload to the frame under construction, emitting the finished
	// frame into `result` when the E bit arrives. Returns false if the packet was dropped.
	bool accumulate(StreamState &stream, const message_ptr &packet, size_t hdrSize,
	                size_t payloadEnd, message_vector &result);
	void discardPartial(StreamState &stream, const char *reason);

	// One decoder per RTP stream with per-SSRC derivation on, otherwise one for the m-line; see
	// SFrameDecoderSet.
	unique_ptr<impl::SFrameDecoderSet> mSFrameDecoders;

	// A peer can start a frame and never finish it, so the buffer is capped. Well above
	// any real audio frame: at the packetizer's chunk size this is several packets'
	// worth, and Opus tops out at 1275 bytes.
	inline static const size_t MaxPartialSize = 64 * 1024;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_SFRAME_PER_FRAME_AUDIO_RTP_DEPACKETIZER_H */
