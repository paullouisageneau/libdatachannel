/**
 * Copyright (c) 2025
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_GOOGLE_VLA_EXT_H
#define RTC_GOOGLE_VLA_EXT_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace rtc {

// Google Video Layer Allocation for simulcast
//
// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/video-layers-allocation00

struct RTC_CPP_EXPORT GoogleVideoLayerAllocation {
	struct SpatialLayer {
		uint16_t width = 0;
		uint16_t height = 0;
		uint8_t fps = 0;
		std::vector<uint32_t> targetBitratesKbps;  // per temporal layer, cumulative
	};

	struct RtpStream {
		std::vector<SpatialLayer> spatialLayers;
	};

	std::vector<RtpStream> rtpStreams;  // up to 4 streams
};

/// Generate the wire format for Google Video Layer Allocation RTP header extension
/// @param allocation The layer allocation data
/// @param streamIndex The RTP stream index (0-3) for this packet's stream
/// @return Binary payload for the RTP header extension, empty if allocation is invalid
binary generateGoogleVideoLayerAllocation(
    const std::shared_ptr<const GoogleVideoLayerAllocation>& allocation,
    uint8_t streamIndex);

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_GOOGLE_VLA_EXT_H */
