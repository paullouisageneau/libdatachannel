/**
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_VIDEO_LAYERS_ALLOCATION_H
#define RTC_VIDEO_LAYERS_ALLOCATION_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace rtc {

// Google Video Layers Allocation for simulcast
//
// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/video-layers-allocation00

struct RTC_CPP_EXPORT VideoLayersAllocation {
	struct SpatialLayer {
		uint16_t width = 0;
		uint16_t height = 0;
		uint8_t fps = 0;
		std::vector<uint32_t> targetBitratesKbps;  // per temporal layer, cumulative, cannot be empty
	};

	struct RtpStream {
		std::vector<SpatialLayer> spatialLayers;
	};

	std::vector<RtpStream> rtpStreams;  // up to 4 streams

	/// Generate the wire format for Google Video Layers Allocation RTP header extension
	/// @param streamIndex The RTP stream index (0-3) for this packet's stream
	/// @return Binary payload for the RTP header extension, empty if allocation is invalid
	binary generate(uint8_t streamIndex) const;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_VIDEO_LAYERS_ALLOCATION_H */
