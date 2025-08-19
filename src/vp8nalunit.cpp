#if RTC_ENABLE_MEDIA

#include "vp8nalunit.hpp"
#include <stdexcept>
#include <algorithm>

namespace rtc {

size_t VP8NalUnit::parseDescriptor()
{
	// If we parse repeatedly, that’s OK, but we re-check from the start each time.
	if (empty()) {
		return 0;
	}
	const size_t totalSize = size();
	size_t offset = 0;

	// Access our underlying data() as bytes
	const std::byte* rawPtr = data();

	// Read the mandatory first descriptor byte
	if (offset + 1 > totalSize) {
		return 0;
	}
	uint8_t fbVal = std::to_integer<uint8_t>(rawPtr[offset]);
	mFirstByte.raw = fbVal;
	offset++;

	mHasExtension = mFirstByte.hasExtension();

	// If the extension bit X=1, parse next byte
	if (mHasExtension && offset < totalSize) {
		uint8_t extVal = std::to_integer<uint8_t>(rawPtr[offset]);
		mExtByte.raw = extVal;
		offset++;

		// If I=1 => parse PictureID
		if (mExtByte.hasPictureID() && offset < totalSize) {
			uint8_t pidByte1 = std::to_integer<uint8_t>(rawPtr[offset]);
			offset++;

			bool mBit = (pidByte1 & 0x80) != 0;    // top bit
			uint8_t high7 = (pidByte1 & 0x7F);
			mPictureID = high7;
			mHasPictureID = true;

			if (mBit && offset < totalSize) {
				uint8_t pidByte2 = std::to_integer<uint8_t>(rawPtr[offset]);
				offset++;
				mPictureID = (uint16_t)((high7 << 8) | pidByte2);
			}
		}

		// If L=1 => we have 1 byte TL0PICIDX
		if (mExtByte.hasTL0PICIDX() && offset < totalSize) {
			offset++; // skip or store as needed
		}

		// If T=1 or K=1 => we have 1 byte TID|Y|KEYIDX
		if ((mExtByte.hasTID() || mExtByte.hasKEYIDX()) && offset < totalSize) {
			offset++; // skip or store
		}
	}

	// If this is the start of partition 0, we can check the “P bit” in the next 3 bytes
	// (the “uncompressed data chunk” start) to decide if it's a key frame. 
	if (mFirstByte.isStartOfPartition() && (mFirstByte.partitionIndex() == 0)) {
		// Must have at least 3 more bytes for the VP8 payload header
		if (offset + 3 <= totalSize) {
			uint8_t b0 = std::to_integer<uint8_t>(rawPtr[offset]);
			// The bottom bit of b0 => “P” bit: 0 => keyframe, 1 => interframe
			bool pBit = (b0 & 0x01) != 0;
			mIsKeyFrame = !pBit;
		}
	}

	return offset;
}

binary VP8NalUnit::payload() const
{
	// We want to return the portion after the descriptor. 
	VP8NalUnit* self = const_cast<VP8NalUnit*>(this);
	size_t descLen = self->parseDescriptor();
	if (descLen >= size()) {
		return {};
	}
	return binary(begin() + descLen, end());
}

std::vector<binary> VP8NalUnit::GenerateFragments(const std::vector<VP8NalUnit> &units,
                                                  size_t maxFragmentSize)
{
	std::vector<binary> output;
	for (auto &u : units) {
		if (u.size() <= maxFragmentSize) {
			output.push_back(u);
		} else {
			auto frags = u.generateFragments(maxFragmentSize);
			for (auto &f : frags) {
				output.push_back(std::move(f));
			}
		}
	}
	return output;
}

std::vector<VP8NalUnit> VP8NalUnit::generateFragments(size_t maxFragmentSize) const
{
	// Not a standardized approach for VP8, but parallels your H.265 code.
	std::vector<VP8NalUnit> result;
	if (size() <= maxFragmentSize) {
		result.push_back(*this);
		return result;
	}

	// First, parse descriptor to see how many bytes it consumes:
	VP8NalUnit tmp(*this);
	size_t descLen = tmp.parseDescriptor();
	if (descLen >= size()) {
		// Malformed or no real payload
		result.push_back(*this);
		return result;
	}

	// Copy out the descriptor bytes
	binary descriptor(begin(), begin() + descLen);
	// Copy out the actual compressed payload
	binary vp8Data(begin() + descLen, end());

	// We'll loop over vp8Data in chunks up to (maxFragmentSize - descriptor.size()) 
	// so that each fragment is <= maxFragmentSize total.
	size_t offset = 0;
	while (offset < vp8Data.size()) {
		size_t spaceForPayload =
		    (descriptor.size() >= maxFragmentSize)
		        ? 0
		        : (maxFragmentSize - descriptor.size());
		size_t chunk = std::min(spaceForPayload, vp8Data.size() - offset);

		binary fragData;
		fragData.reserve(descriptor.size() + chunk);

		// If we’re not on the very first chunk, we must clear the 'S' bit 
		// in the first descriptor byte. Because only the first fragment is 
		// the "start of partition."
		if (offset != 0) {
			// Make a temporary copy of descriptor so we do not permanently change it.
			binary descCopy(descriptor);
			uint8_t d0 = std::to_integer<uint8_t>(descCopy[0]);
			d0 &= ~(1 << 4); // clear S bit
			descCopy[0] = std::byte(d0);

			fragData.insert(fragData.end(), descCopy.begin(), descCopy.end());
		} else {
			// First chunk gets the descriptor unchanged
			fragData.insert(fragData.end(), descriptor.begin(), descriptor.end());
		}

		// Append the chunk of actual VP8 data
		fragData.insert(fragData.end(),
		                vp8Data.begin() + offset,
		                vp8Data.begin() + offset + chunk);

		offset += chunk;
		result.emplace_back(std::move(fragData)); // calls VP8NalUnit(binary&&) constructor
	}

	return result;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */