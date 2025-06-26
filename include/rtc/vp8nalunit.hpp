#ifndef RTC_VP8_NAL_UNIT_H
#define RTC_VP8_NAL_UNIT_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "nalunit.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

/*
 * Example “VP8NalUnit” derived from your base NalUnit class, mirroring
 * how "H265NalUnit" is done. In VP8, there is no true NAL unit concept,
 * but we can still parse the "VP8 payload descriptor" (RFC 7741 sec. 4.2)
 * from the front of the data, and store or retrieve key bits.
 * 
 * IMPORTANT: We avoid calling NalUnit(std::move(data), Type) since
 * NalUnit has no such two-argument constructor. Instead, we do
 * NalUnit(std::move(data)) just like your H265 code.
 */

namespace rtc {

#pragma pack(push, 1)

// The mandatory first byte of the VP8 payload descriptor.
// (RFC 7741, Section 4.2)
struct VP8PayloadDescriptorFirstByte {
	uint8_t raw = 0;

	bool hasExtension()     const { return (raw & 0x80) != 0; } // X
	bool isNonReference()   const { return (raw & 0x20) != 0; } // N
	bool isStartOfPartition() const { return (raw & 0x10) != 0; } // S
	uint8_t partitionIndex() const { return (raw & 0x07); } // PID

	void setHasExtension(bool val) {
		if(val) raw |=  (1 << 7);
		else     raw &= ~(1 << 7);
	}
	void setNonReference(bool val) {
		if(val) raw |=  (1 << 5);
		else     raw &= ~(1 << 5);
	}
	void setStartOfPartition(bool val) {
		if(val) raw |=  (1 << 4);
		else     raw &= ~(1 << 4);
	}
	void setPartitionIndex(uint8_t pid) {
		raw = (raw & 0xF8) | (pid & 0x07);
	}
};

// Optional extension byte if the X bit = 1.
struct VP8PayloadDescriptorExtensionByte {
	uint8_t raw = 0;

	bool hasPictureID() const { return (raw & 0x80) != 0; } // I
	bool hasTL0PICIDX() const { return (raw & 0x40) != 0; } // L
	bool hasTID()      const { return (raw & 0x20) != 0; } // T
	bool hasKEYIDX()   const { return (raw & 0x10) != 0; } // K
};

#pragma pack(pop)

// “VP8NalUnit” extends NalUnit but is really a container for one VP8 frame
// (or partial frame) plus its RTP "payload descriptor" bits.
struct VP8NalUnit : public NalUnit {
public:
	// Match your H265NalUnit style:
	VP8NalUnit() : NalUnit(NalUnit::Type::VP8) {}
	VP8NalUnit(size_t size, bool includingHeader = true)
		: NalUnit(size, includingHeader, NalUnit::Type::VP8) {}
	VP8NalUnit(binary &&data)
		: NalUnit(std::move(data)) {} // call NalUnit(binary && data)
	VP8NalUnit(const VP8NalUnit&) = default;

	// Parse the VP8 payload descriptor from the beginning of data().
	// Returns how many bytes of descriptor were consumed.
	size_t parseDescriptor();

	// The actual VP8 bitstream data after the descriptor.
	binary payload() const;

	// If this frame is a keyframe or not (based on the "P" bit in the
	// first 3 bytes of the actual VP8 payload when partitionIndex=0).
	bool isKeyFrame() const { return mIsKeyFrame; }

	// The “S bit” in the first descriptor byte.
	bool isStartOfPartition() const { return mFirstByte.isStartOfPartition(); }
	
	VP8PayloadDescriptorFirstByte mFirstByte {0};

	// PictureID if present in the extension. (RFC 7741 Section 4.2)
	uint16_t pictureID() const { return mPictureID; }

	// For demonstration, generating multiple “fragments” from a large frame
	// simulating the style of H.265 FU logic, though not standardized for VP8:
	static std::vector<binary> GenerateFragments(const std::vector<VP8NalUnit> &units,
	                                             size_t maxFragmentSize);
	std::vector<VP8NalUnit> generateFragments(size_t maxFragmentSize) const;

private:
	bool mHasExtension = false;
	VP8PayloadDescriptorExtensionByte mExtByte {0};

	bool     mHasPictureID = false;
	uint16_t mPictureID    = 0;
	bool     mIsKeyFrame   = false; // if “P=0” in the first-byte of actual VP8 data

	// Implementation detail: parse out extension fields if X=1, etc.
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_VP8_NAL_UNIT_H */