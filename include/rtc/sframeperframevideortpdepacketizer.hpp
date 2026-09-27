/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_PER_FRAME_VIDEO_RTP_DEPACKETIZER_H
#define RTC_SFRAME_PER_FRAME_VIDEO_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtpdepacketizer.hpp"
#include "sframe.hpp"

#include <map>
#include <memory>
#include <vector>

namespace rtc {

/// RTP depacketizer for SFrame RFC mode on video tracks
/// (draft-ietf-avtcore-rtp-sframe).
///
/// Strips the 1-byte S/E/T descriptor and joins each frame's payloads back together, bounded by
/// the descriptor's S and E bits rather than by the RTP marker bit. No codec-specific NAL
/// reassembly is performed.
///
/// A key provider is required at construction, so a frame carrying SFrame is never handed upstream
/// undecrypted. Nor is a plaintext one: this depacketizer always decrypts, because an m-line whose
/// answer declined a=sframe is stopped rather than downgraded, so it is never installed on a track
/// carrying media in the clear.
class RTC_CPP_EXPORT SFramePerFrameVideoRtpDepacketizer final : public VideoRtpDepacketizer {
public:
	/// Throws std::invalid_argument if keyProvider is null.
	/// @param keyProvider Supplies the key for each generation, and fixes the session's
	///                    cipher suite, KID layout and per-SSRC derivation setting
	SFramePerFrameVideoRtpDepacketizer(shared_ptr<SFrameReceiveKeyProvider> keyProvider);
	~SFramePerFrameVideoRtpDepacketizer();

	void incoming(message_vector &messages, const message_callback &send) override;


	/// This handler applies SFrame, so an answer for this m-line may assert "a=sframe".
	bool appliesSFrame() const override { return true; }

private:
	// Unreachable: incoming() is fully overridden, so the base never calls this. Present only
	// because VideoRtpDepacketizer declares it pure virtual.
	message_ptr reassemble(message_buffer &buffer) override;

	bool sframeGroupResolves(const message_buffer &packets);

	// Why a group did not fully resolve, so loss and corruption are counted apart.
	struct SFrameOutcome {
		bool allConsumed = true;
		bool sawLoss = false;      // a sequence number that never arrived
		bool sawMalformed = false; // unparseable, or a descriptor that cannot be honoured
	};
	SFrameOutcome assembleSFrameObjects(const message_buffer &packets, message_vector &result,
	                                    bool dryRun = false);
	optional<size_t> validateSFrameRun(const message_vector &run);
	bool assembleSFrameObject(const message_vector &run, message_vector &result);

	// One decoder per RTP stream with per-SSRC derivation on, otherwise one for the m-line; see
	// SFrameDecoderSet.
	unique_ptr<impl::SFrameDecoderSet> mSFrameDecoders;

	// One RTP timestamp's packets. Held in the order the groups were first seen rather than in
	// timestamp order -- see flushSFramesExceptNewest() in the .cpp.
	struct SFrameGroup {
		uint32_t timestamp;
		uint64_t arrival; // global arrival order, to evict the oldest group across all streams
		message_buffer packets;
	};

	// Every SSRC on an m-line resolves to one track, so simulcast, RTX and FEC all arrive here.
	// Their timestamp and sequence spaces are unrelated, so reassembly and the staleness watermark
	// are held per stream: shared, one stream's ordinary timestamps read as stale against
	// another's, and groups sharing a timestamp would be spliced together.
	struct StreamState {
		std::vector<SFrameGroup> groups;
		uint32_t lastEmitted = 0;
		bool emitted = false;
		uint64_t lastArrival = 0; // to evict the stream idle longest
	};
	std::map<SSRC, StreamState> mStreams;

	StreamState &streamFor(SSRC ssrc);
	bool isStaleSFrameTimestamp(const StreamState &stream, uint32_t timestamp) const;
	bool bufferSFramePacket(StreamState &stream, message_ptr packet, uint32_t timestamp);
	void releaseSFrameGroup(StreamState &stream, size_t index, message_vector &result);
	void dropSFrameGroup(StreamState &stream, size_t index);
	void flushSFramesThrough(StreamState &stream, uint32_t timestamp, message_vector &result);
	void flushSFramesExceptNewest(StreamState &stream, message_vector &result);
	void emitCompleteSFrames(StreamState &stream, message_vector &result);
	void evictOverflowingSFrames();

	// Totals across every stream, so a peer announcing many SSRCs cannot multiply the budget.
	size_t mSFrameBufferedBytes = 0;
	uint64_t mSFrameArrivals = 0;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_SFRAME_PER_FRAME_VIDEO_RTP_DEPACKETIZER_H */
