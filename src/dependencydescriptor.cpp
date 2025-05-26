/**
 * Copyright (c) 2024 Shigemasa Watanabe (Wandbox)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dependencydescriptor.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <stdexcept>

namespace rtc {

BitWriter BitWriter::fromSizeBits(byte *buf, size_t offsetBits, size_t sizeBits) {
	BitWriter writer;
	writer.mBuf = buf;
	writer.mInitialOffset = offsetBits;
	writer.mOffset = offsetBits;
	writer.mSize = sizeBits;
	return writer;
}
BitWriter BitWriter::fromNull() {
	BitWriter writer;
	writer.mSize = std::numeric_limits<size_t>::max();
	return writer;
}

size_t BitWriter::getWrittenBits() const { return mOffset - mInitialOffset; }

bool BitWriter::write(uint64_t v, size_t bits) {
	if (mOffset + bits > mSize) {
		return false;
	}
	uint8_t *p = mBuf == nullptr ? nullptr : reinterpret_cast<uint8_t *>(mBuf + mOffset / 8);
	// First, write up to the 8-bit boundary
	size_t written_bits = writePartialByte(p, mOffset % 8, v, bits);

	if (p != nullptr) {
		p++;
	}
	bits -= written_bits;
	mOffset += written_bits;

	if (bits == 0) {
		return true;
	}

	// Write 8 bits at a time
	while (bits >= 8) {
		if (p != nullptr) {
			*p = (v >> (bits - 8)) & 0xff;
			p++;
		}
		bits -= 8;
		mOffset += 8;
	}

	// Write the remaining bits
	written_bits = writePartialByte(p, 0, v, bits);
	bits -= written_bits;
	mOffset += written_bits;

	assert(bits == 0);

	return true;
}

bool BitWriter::writeNonSymmetric(uint64_t v, uint64_t n) {
	if (n == 1) {
		return true;
	}
	size_t w = 0;
	uint64_t x = n;
	while (x != 0) {
		x = x >> 1;
		w++;
	}
	uint64_t m = (1ULL << w) - n;
	if (v < m) {
		return write(v, w - 1);
	} else {
		return write(v + m, w);
	}
}

size_t BitWriter::writePartialByte(uint8_t *p, size_t offset, uint64_t v, size_t bits) {
	// How many bits are remaining
	size_t remaining_bits = 8 - offset;
	// Number of bits to write
	size_t need_write_bits = std::min(remaining_bits, bits);
	// Number of remaining bits
	size_t shift = remaining_bits - need_write_bits;
	// The relationship between each values are as follows
	// 0bxxxxxxxx
	//   ^        - offset == 1
	//    ^-----^ - remaining_bits == 7
	//    ^---^   - need_write_bits == 5
	//         ^^ - shift == 2
	assert(offset + remaining_bits == 8);
	assert(remaining_bits == need_write_bits + shift);

	// For writing 4 bits from the 3rd bit of 0bxxxxxxxx with 0byyyy, it becomes
	// (0bxxxxxxxx & 0b11100001) | ((0byyyy >> (4 - 4)) << 1)
	// For writing 2 bits from the 6th bit of 0bxxxxxxxx with 0byyyyy, it becomes
	// (0bxxxxxxxx & 0b11111100) | (((0byyyyy >> (5 - 2)) << 0)

	// Creating a mask
	// For need_write_bits == 4, shift == 1
	// 1 << 4 == 0b00010000
	// 0b00010000 - 1 == 0b00001111
	// 0b00001111 << 1 == 0b00011110
	// ~0b00011110 == 0b11100001
	uint8_t mask = ~(((1 << need_write_bits) - 1) << shift);

	uint8_t vv = static_cast<uint8_t>(v >> (bits - need_write_bits));

	if (p != nullptr) {
		*p = (*p & mask) | (vv << shift);
	}

	return need_write_bits;
}

using TemplateIterator = std::vector<FrameDependencyTemplate>::const_iterator;

struct TemplateMatch {
	size_t templatePosition;
	bool needCustomDtis;
	bool needCustomFdiffs;
	bool needCustomChains;
	// Size in bits to store frame-specific details, i.e.
	// excluding mandatory fields and template dependency structure.
	size_t extraSizeBits;
};

static TemplateMatch calculate_match(TemplateIterator frameTemplate,
                                     const FrameDependencyStructure &structure,
                                     std::bitset<32> activeChains,
                                     const DependencyDescriptor &descriptor) {
	TemplateMatch result;
	result.templatePosition = frameTemplate - structure.templates.begin();
	result.needCustomFdiffs = descriptor.dependencyTemplate.frameDiffs != frameTemplate->frameDiffs;
	result.needCustomDtis = descriptor.dependencyTemplate.decodeTargetIndications !=
	                        frameTemplate->decodeTargetIndications;
	result.needCustomChains = false;
	for (int i = 0; i < structure.chainCount; ++i) {
		if (activeChains[i] &&
		    descriptor.dependencyTemplate.chainDiffs[i] != frameTemplate->chainDiffs[i]) {
			result.needCustomChains = true;
			break;
		}
	}

	result.extraSizeBits = 0;
	if (result.needCustomFdiffs) {
		result.extraSizeBits += 2 * (1 + descriptor.dependencyTemplate.frameDiffs.size());
		for (int fdiff : descriptor.dependencyTemplate.frameDiffs) {
			if (fdiff <= (1 << 4)) {
				result.extraSizeBits += 4;
			} else if (fdiff <= (1 << 8)) {
				result.extraSizeBits += 8;
			} else {
				result.extraSizeBits += 12;
			}
		}
	}
	if (result.needCustomDtis) {
		result.extraSizeBits += 2 * descriptor.dependencyTemplate.decodeTargetIndications.size();
	}
	if (result.needCustomChains) {
		result.extraSizeBits += 8 * structure.chainCount;
	}
	return result;
}

static bool find_best_template(const FrameDependencyStructure &structure,
                               std::bitset<32> activeChains, const DependencyDescriptor &descriptor,
                               TemplateMatch *best) {
	auto &templates = structure.templates;
	// Find range of templates with matching spatial/temporal id.
	auto sameLayer = [&](const FrameDependencyTemplate &frameTemplate) {
		return descriptor.dependencyTemplate.spatialId == frameTemplate.spatialId &&
		       descriptor.dependencyTemplate.temporalId == frameTemplate.temporalId;
	};
	auto first = std::find_if(templates.begin(), templates.end(), sameLayer);
	if (first == templates.end()) {
		return false;
	}
	auto last = std::find_if_not(first, templates.end(), sameLayer);

	*best = calculate_match(first, structure, activeChains, descriptor);
	// Search if there any better template than the first one.
	for (auto next = std::next(first); next != last; ++next) {
		auto match = calculate_match(next, structure, activeChains, descriptor);
		if (match.extraSizeBits < best->extraSizeBits) {
			*best = match;
		}
	}
	return true;
}

static const uint32_t MaxTemplates = 64;

DependencyDescriptorWriter::DependencyDescriptorWriter(const DependencyDescriptorContext &context)
    : mStructure(context.structure), mActiveChains(context.activeChains),
      mDescriptor(context.descriptor) {}

size_t DependencyDescriptorWriter::getSizeBits() const {
	auto writer = rtc::BitWriter::fromNull();
	doWriteTo(writer);
	return writer.getWrittenBits();
}
size_t DependencyDescriptorWriter::getSize() const { return (getSizeBits() + 7) / 8; }

void DependencyDescriptorWriter::writeTo(byte *buf, size_t sizeBytes) const {
	auto writer = BitWriter::fromSizeBits(buf, 0, sizeBytes * 8);
	doWriteTo(writer);
	// Pad up to the byte boundary
	if (auto bits = (writer.getWrittenBits() % 8); bits != 0) {
		writer.write(0, 8 - bits);
	}
}

void DependencyDescriptorWriter::doWriteTo(BitWriter &w) const {
	TemplateMatch bestTemplate;
	if (!find_best_template(mStructure, mActiveChains, mDescriptor, &bestTemplate)) {
		throw std::logic_error("No matching template found");
	}

	// mandatory_descriptor_fields()
	writeBits(w, mDescriptor.startOfFrame ? 1 : 0, 1);
	writeBits(w, mDescriptor.endOfFrame ? 1 : 0, 1);
	uint32_t templateId =
	    (bestTemplate.templatePosition + mStructure.templateIdOffset) % MaxTemplates;
	writeBits(w, templateId, 6);
	writeBits(w, mDescriptor.frameNumber, 16);

	bool hasExtendedFields = bestTemplate.extraSizeBits > 0 ||
	                         (mDescriptor.startOfFrame && mDescriptor.structureAttached) ||
	                         mDescriptor.activeDecodeTargetsBitmask != std::nullopt;
	if (hasExtendedFields) {
		// extended_descriptor_fields()
		bool templateDependencyStructurePresentFlag = mDescriptor.structureAttached;
		writeBits(w, templateDependencyStructurePresentFlag ? 1 : 0, 1);
		bool activeDecodeTargetsPresentFlag = std::invoke([&]() {
			if (!mDescriptor.activeDecodeTargetsBitmask)
				return false;
			const uint64_t allDecodeTargetsBitmask = (1ULL << mStructure.decodeTargetCount) - 1;
			if (mDescriptor.structureAttached &&
			    mDescriptor.activeDecodeTargetsBitmask == allDecodeTargetsBitmask)
				return false;
			return true;
		});
		writeBits(w, activeDecodeTargetsPresentFlag ? 1 : 0, 1);
		writeBits(w, bestTemplate.needCustomDtis ? 1 : 0, 1);
		writeBits(w, bestTemplate.needCustomFdiffs ? 1 : 0, 1);
		writeBits(w, bestTemplate.needCustomChains ? 1 : 0, 1);
		if (templateDependencyStructurePresentFlag) {
			// template_dependency_structure()
			writeBits(w, mStructure.templateIdOffset, 6);
			writeBits(w, mStructure.decodeTargetCount - 1, 5);

			// template_layers()
			const auto &templates = mStructure.templates;
			assert(!templates.empty());
			assert(templates.size() < MaxTemplates);
			assert(templates[0].spatialId == 0);
			assert(templates[0].temporalId == 0);
			for (size_t i = 1; i < templates.size(); ++i) {
				auto &prev = templates[i - 1];
				auto &next = templates[i];

				uint32_t nextLayerIdc;
				if (next.spatialId == prev.spatialId && next.temporalId == prev.temporalId) {
					// same layer
					nextLayerIdc = 0;
				} else if (next.spatialId == prev.spatialId &&
				           next.temporalId == prev.temporalId + 1) {
					// next temporal
					nextLayerIdc = 1;
				} else if (next.spatialId == prev.spatialId + 1 && next.temporalId == 0) {
					// new spatial
					nextLayerIdc = 2;
				} else {
					throw std::logic_error("Invalid layer");
				}
				writeBits(w, nextLayerIdc, 2);
			}
			// no more layers
			writeBits(w, 3, 2);

			// template_dtis()
			for (const FrameDependencyTemplate &frameTemplate : mStructure.templates) {
				assert(frameTemplate.decodeTargetIndications.size() ==
				       static_cast<size_t>(mStructure.decodeTargetCount));
				for (DecodeTargetIndication dti : frameTemplate.decodeTargetIndications) {
					writeBits(w, static_cast<uint64_t>(dti), 2);
				}
			}

			// template_fdiffs()
			for (const FrameDependencyTemplate &frameTemplate : mStructure.templates) {
				for (int fdiff : frameTemplate.frameDiffs) {
					assert(fdiff - 1 >= 0);
					assert(fdiff - 1 < (1 << 4));
					writeBits(w, (1u << 4) | (fdiff - 1), 1 + 4);
				}
				// No more diffs for current template.
				writeBits(w, 0, 1);
			}

			// template_chains()
			assert(mStructure.chainCount >= 0);
			assert(mStructure.chainCount <= mStructure.decodeTargetCount);
			writeNonSymmetric(w, mStructure.chainCount, mStructure.decodeTargetCount + 1);
			if (mStructure.chainCount != 0) {
				assert(mStructure.decodeTargetProtectedBy.size() ==
				       static_cast<size_t>(mStructure.decodeTargetCount));
				for (int protectedBy : mStructure.decodeTargetProtectedBy) {
					assert(protectedBy >= 0);
					assert(protectedBy < mStructure.chainCount);
					writeNonSymmetric(w, protectedBy, mStructure.chainCount);
				}
				for (const auto &frameTemplate : mStructure.templates) {
					assert(frameTemplate.chainDiffs.size() ==
					       static_cast<size_t>(mStructure.chainCount));
					for (int chain_diff : frameTemplate.chainDiffs) {
						assert(chain_diff >= 0);
						assert(chain_diff < (1 << 4));
						writeBits(w, chain_diff, 4);
					}
				}
			}

			bool hasResolutions = !mStructure.resolutions.empty();
			writeBits(w, hasResolutions ? 1 : 0, 1);
			if (hasResolutions) {
				// render_resolutions()
				assert(mStructure.resolutions.size() ==
				       static_cast<size_t>(mStructure.templates.back().spatialId) + 1);
				for (const RenderResolution &resolution : mStructure.resolutions) {
					assert(resolution.width > 0);
					assert(resolution.width <= (1 << 16));
					assert(resolution.height > 0);
					assert(resolution.height <= (1 << 16));

					writeBits(w, resolution.width - 1, 16);
					writeBits(w, resolution.height - 1, 16);
				}
			}
		}
		if (activeDecodeTargetsPresentFlag) {
			writeBits(w, *mDescriptor.activeDecodeTargetsBitmask, mStructure.decodeTargetCount);
		}
	}

	// frame_dependency_definition()
	if (bestTemplate.needCustomDtis) {
		// frame_dtis()
		assert(mDescriptor.dependencyTemplate.decodeTargetIndications.size() ==
		       static_cast<size_t>(mStructure.decodeTargetCount));
		for (DecodeTargetIndication dti : mDescriptor.dependencyTemplate.decodeTargetIndications) {
			writeBits(w, static_cast<uint32_t>(dti), 2);
		}
	}
	if (bestTemplate.needCustomFdiffs) {
		// frame_fdiffs()
		for (int fdiff : mDescriptor.dependencyTemplate.frameDiffs) {
			assert(fdiff > 0);
			assert(fdiff <= (1 << 12));
			if (fdiff <= (1 << 4)) {
				writeBits(w, (1u << 4) | (fdiff - 1), 2 + 4);
			} else if (fdiff <= (1 << 8)) {
				writeBits(w, (2u << 8) | (fdiff - 1), 2 + 8);
			} else { // fdiff <= (1 << 12)
				writeBits(w, (3u << 12) | (fdiff - 1), 2 + 12);
			}
		}
		// No more diffs.
		writeBits(w, 0, 2);
	}
	if (bestTemplate.needCustomChains) {
		// frame_chains()
		for (int i = 0; i < mStructure.chainCount; ++i) {
			int chainDiff = mActiveChains[i] ? mDescriptor.dependencyTemplate.chainDiffs[i] : 0;
			assert(chainDiff >= 0);
			assert(chainDiff < (1 << 8));
			writeBits(w, chainDiff, 8);
		}
	}
}
void DependencyDescriptorWriter::writeBits(BitWriter &writer, uint64_t v, size_t bits) const {
	if (!writer.write(v, bits)) {
		throw std::logic_error("Failed to write bits");
	}
}
void DependencyDescriptorWriter::writeNonSymmetric(BitWriter &writer, uint64_t v,
                                                   uint64_t n) const {
	if (!writer.writeNonSymmetric(v, n)) {
		throw std::logic_error("Failed to write non-symmetric value");
	}
}

} // namespace rtc
