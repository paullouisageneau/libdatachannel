/**
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "video_layers_allocation.hpp"

#include <algorithm>

namespace rtc {

namespace {

// Encode unsigned integer as LEB128
void writeLeb128(binary& out, uint32_t value) {
	do {
		uint8_t byte = value & 0x7F;
		value >>= 7;
		if (value != 0) {
			byte |= 0x80;  // More bytes follow
		}
		out.push_back(std::byte(byte));
	} while (value != 0);
}

// Compute spatial layer bitmask for a stream
// Returns bitmask where bit i is set if spatial layer i is present
uint8_t computeSpatialLayerBitmask(const VideoLayersAllocation::RtpStream& stream) {
	uint8_t bitmask = 0;
	for (size_t i = 0; i < stream.spatialLayers.size() && i < 4; ++i) {
		const auto& spatialLayer = stream.spatialLayers[i];
		if (!spatialLayer.targetBitratesKbps.empty()) {
			bitmask |= (1 << i);
		}
	}
	return bitmask;
}

} // namespace

binary VideoLayersAllocation::generate(uint8_t streamIndex) const {
	const auto numStreams = std::min<size_t>(rtpStreams.size(), 4u);
	if (numStreams == 0 || streamIndex >= numStreams) {
		return {};
	}

	binary result;
	result.reserve(60);  // Pre-allocate reasonable size

	// Compute spatial layer bitmasks for all streams
	std::vector<uint8_t> slBitmasks;
	slBitmasks.reserve(numStreams);
	for (size_t i = 0; i < numStreams; ++i) {
		slBitmasks.push_back(computeSpatialLayerBitmask(rtpStreams[i]));
	}

	// Check if all streams have the same spatial layer bitmask
	bool allSameBitmask = true;
	for (size_t i = 1; i < numStreams; ++i) {
		if (slBitmasks[i] != slBitmasks[0]) {
			allSameBitmask = false;
			break;
		}
	}

	// Check if we have any active spatial layers in any streams
	if (allSameBitmask && slBitmasks[0] == 0) {
		return {};
	}

	// Header byte: RID(2) | NS(2) | sl_bm(4)
	uint8_t rid = streamIndex & 0x03;
	uint8_t ns = (numStreams - 1) & 0x03;
	uint8_t slBm = allSameBitmask ? slBitmasks[0] : 0;

	uint8_t headerByte = (rid << 6) | (ns << 4) | (slBm & 0x0F);
	result.push_back(std::byte(headerByte));

	// If sl_bm == 0, write per-stream spatial layer bitmasks
	// Each slX_bm is 4 bits, packed and zero-padded to byte boundary
	if (slBm == 0) {
		for (size_t i = 0; i < numStreams; i += 2) {
			uint8_t byte = (slBitmasks[i] & 0x0F) << 4;
			if (i + 1 < numStreams) {
				byte |= (slBitmasks[i + 1] & 0x0F);
			}
			result.push_back(std::byte(byte));
		}
	}

	// Temporal layer counts: 2 bits per active spatial layer across all streams
	// Value is (num_temporal_layers - 1), so 0 = 1 TL, 1 = 2 TL, 2 = 3 TL, 3 = 4 TL
	uint8_t tempByte = 0;
	int bitPos = 6;  // Start from MSB, 2 bits at a time

	for (size_t streamIdx = 0; streamIdx < numStreams; ++streamIdx) {
		uint8_t bitmask = allSameBitmask ? slBm : slBitmasks[streamIdx];
		const auto& stream = rtpStreams[streamIdx];

		for (size_t slIdx = 0; slIdx < stream.spatialLayers.size() && slIdx < 4; ++slIdx) {
			if (bitmask & (1 << slIdx)) {
				const auto& sl = stream.spatialLayers[slIdx];
				const auto numTemporal = std::min<size_t>(sl.targetBitratesKbps.size(), 4u);
				const auto tlValue = uint8_t((numTemporal - 1) & 0x03);

				tempByte |= (tlValue << bitPos);
				bitPos -= 2;

				if (bitPos < 0) {
					result.push_back(std::byte(tempByte));
					tempByte = 0;
					bitPos = 6;
				}
			}
		}
	}

	// Flush remaining temporal layer bits if any
	if (bitPos != 6) {
		result.push_back(std::byte(tempByte));
	}

	// Target bitrates in kbps, LEB128 encoded
	// Order: for each stream, for each spatial layer (by id), for each temporal layer
	for (size_t streamIdx = 0; streamIdx < numStreams; ++streamIdx) {
		const auto bitmask = allSameBitmask ? slBm : slBitmasks[streamIdx];
		const auto& stream = rtpStreams[streamIdx];

		for (size_t slIdx = 0; slIdx < stream.spatialLayers.size() && slIdx < 4; ++slIdx) {
			if ((bitmask & (1 << slIdx)) != 0) {
				const auto& sl = stream.spatialLayers[slIdx];
				for (uint32_t bitrate : sl.targetBitratesKbps) {
					writeLeb128(result, bitrate);
				}
			}
		}
	}

	// Resolution and framerate: 5 bytes per active spatial layer
	// Format: width-1 (2 bytes BE), height-1 (2 bytes BE), fps (1 byte)
	for (size_t streamIdx = 0; streamIdx < numStreams; ++streamIdx) {
		const auto bitmask = allSameBitmask ? slBm : slBitmasks[streamIdx];
		const auto& stream = rtpStreams[streamIdx];

		for (size_t slIdx = 0; slIdx < stream.spatialLayers.size() && slIdx < 4; ++slIdx) {
			if (bitmask & (1 << slIdx)) {
				const auto& sl = stream.spatialLayers[slIdx];
				uint16_t width = std::max<uint32_t>(sl.width, 1u) - 1u;
				uint16_t height = std::max<uint32_t>(sl.height, 1u) - 1u;

				// Big endian width-1
				result.push_back(std::byte((width >> 8) & 0xFF));
				result.push_back(std::byte(width & 0xFF));
				// Big endian height-1
				result.push_back(std::byte((height >> 8) & 0xFF));
				result.push_back(std::byte(height & 0xFF));
				// Framerate
				result.push_back(std::byte(sl.fps));
			}
		}
	}

	return result;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
