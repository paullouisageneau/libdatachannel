/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_SFRAME_VIDEO_RTP_DEPACKETIZER_H
#define RTC_SFRAME_VIDEO_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtpdepacketizer.hpp"
#include "sframe.hpp"

#include <memory>

namespace rtc {

/// RTP depacketizer for SFrame RFC mode on video tracks
/// (draft-ietf-avtcore-rtp-sframe).
///
/// Strips the 1-byte S/E/T descriptor and concatenates payloads across the marker-bit
/// boundary. No codec-specific NAL reassembly is performed.
///
/// A key provider is required at construction, so a frame carrying SFrame is never handed
/// upstream undecrypted. A peer declining a=sframe disables SFrame for the track and its
/// plain payloads are delivered as they arrive -- check Description::Media::hasSFrame() to
/// know whether what you receive was protected.
class RTC_CPP_EXPORT SFrameVideoRtpDepacketizer final : public VideoRtpDepacketizer {
public:
	/// Throws std::invalid_argument if keyProvider is null.
	/// @param keyProvider Supplies per-KID key material to the decoder
	SFrameVideoRtpDepacketizer(std::shared_ptr<SFrameKeyProvider> keyProvider);
	~SFrameVideoRtpDepacketizer();

	void incoming(message_vector &messages, const message_callback &send) override;

	/// Enables or disables SFrame for this m-line from the negotiated media description. A
	/// peer declining it is legal, and packets then pass through as plain codec payloads.
	/// @param desc Negotiated media description
	void media(const Description::Media &desc) override;

	bool handlesSFrame() const override { return true; }

private:
	// Only reached when the peer declined a=sframe: concatenates the plain RTP payloads of
	// one frame. With SFrame enabled, incoming() bypasses reassemble() entirely.
	message_ptr reassemble(message_buffer &buffer) override;

	const std::shared_ptr<SFrameKeyProvider> mKeyProvider;
	std::unique_ptr<SFrameDecoder> mSFrameDecoder;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_SFRAME_VIDEO_RTP_DEPACKETIZER_H */
