/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_AUDIO_RTP_DEPACKETIZER_H
#define RTC_SFRAME_AUDIO_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtpdepacketizer.hpp"
#include "sframe.hpp"

#include <atomic>
#include <memory>

namespace rtc {

/// RTP depacketizer for SFrame RFC mode on audio tracks (draft-ietf-avtcore-rtp-sframe).
///
/// Strips the 1-byte S/E/T descriptor and decrypts. Audio is fragmented on the same MTU
/// boundary as video (a maximum 1275-byte Opus frame already exceeds one chunk), so frames
/// are reassembled from S...E. Fragments are appended in arrival order: one out of sequence
/// or after a gap discards the partial frame rather than being spliced in.
class RTC_CPP_EXPORT SFrameAudioRtpDepacketizer final : public RtpDepacketizer {
public:
	/// Throws std::invalid_argument if keyProvider is null.
	/// @param clockRate RTP clock rate of the audio track
	/// @param keyProvider Supplies per-KID key material to the decoder
	SFrameAudioRtpDepacketizer(uint32_t clockRate, std::shared_ptr<SFrameKeyProvider> keyProvider);
	~SFrameAudioRtpDepacketizer();

	void incoming(message_vector &messages, const message_callback &send) override;

	/// Enables or disables SFrame for this m-line from the negotiated media description. A
	/// peer declining it is legal, and packets then pass through as plain codec payloads.
	/// @param desc Negotiated media description
	void media(const Description::Media &desc) override;

	bool handlesSFrame() const override { return true; }

private:
	// Appends one packet's payload to the frame under construction, emitting the finished
	// frame into `result` when the E bit arrives. Returns false if the packet was dropped.
	bool accumulate(const message_ptr &packet, size_t hdrSize, size_t payloadEnd,
	                message_vector &result);
	void discardPartial(const char *reason);

	// Cleared when the negotiated description carries no a=sframe. Starts set so a
	// depacketizer built directly, without SDP, decrypts as before.
	std::atomic<bool> mSFrameNegotiated{true};

	const std::shared_ptr<SFrameKeyProvider> mKeyProvider;
	std::unique_ptr<SFrameDecoder> mSFrameDecoder;

	// Frame under construction. Empty when no S has been seen since the last E.
	binary mPartial;
	uint32_t mPartialTimestamp = 0;
	uint8_t mPartialPayloadType = 0;

	// Sequence number the next fragment must carry. A gap means a packet was lost and a
	// repeat or a lower value means reordering; either way the fragments cannot be joined.
	uint16_t mNextSeqNumber = 0;

	// A peer can start a frame and never finish it, so the buffer is capped. Well above
	// any real audio frame: at the packetizer's chunk size this is several packets'
	// worth, and Opus tops out at 1275 bytes.
	static constexpr size_t MaxPartialSize = 64 * 1024;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_SFRAME_AUDIO_RTP_DEPACKETIZER_H */
