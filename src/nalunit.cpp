/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "nalunit.hpp"

#include "impl/internals.hpp"

#include <cmath>

namespace rtc {

std::vector<binary> NalUnit::GenerateFragments(const std::vector<NalUnit> &nalus,
                                               size_t maxFragmentSize) {
	std::vector<binary> result;
	for (const auto &nalu : nalus) {
		if (nalu.size() > maxFragmentSize) {
			auto fragments = nalu.generateFragments(maxFragmentSize);
			result.insert(result.end(), fragments.begin(), fragments.end());
		} else {
			// TODO: check this
			result.push_back(nalu);
		}
	}
	return result;
}

std::vector<NalUnitFragmentA> NalUnit::generateFragments(size_t maxFragmentSize) const {
	assert(size() > maxFragmentSize);
	// TODO: check this
	auto fragments_count = ceil(double(size()) / maxFragmentSize);
	maxFragmentSize = uint16_t(int(ceil(size() / fragments_count)));

	// 2 bytes for FU indicator and FU header
	maxFragmentSize -= 2;
	auto f = forbiddenBit();
	uint8_t nri = this->nri() & 0x03;
	uint8_t unitType = this->unitType() & 0x1F;
	auto payload = this->payload();
	size_t offset = 0;
	std::vector<NalUnitFragmentA> result;
	while (offset < payload.size()) {
		vector<byte> fragmentData;
		using FragmentType = NalUnitFragmentA::FragmentType;
		FragmentType fragmentType;
		if (offset == 0) {
			fragmentType = FragmentType::Start;
		} else if (offset + maxFragmentSize < payload.size()) {
			fragmentType = FragmentType::Middle;
		} else {
			if (offset + maxFragmentSize > payload.size()) {
				maxFragmentSize = uint16_t(payload.size() - offset);
			}
			fragmentType = FragmentType::End;
		}
		fragmentData = {payload.begin() + offset, payload.begin() + offset + maxFragmentSize};
		result.emplace_back(fragmentType, f, nri, unitType, fragmentData);
		offset += maxFragmentSize;
	}
	return result;
}

NalUnitStartSequenceMatch NalUnit::StartSequenceMatchSucc(NalUnitStartSequenceMatch match,
                                                          std::byte _byte, Separator separator) {
	assert(separator != Separator::Length);
	auto byte = (uint8_t)_byte;
	auto detectShort =
	    separator == Separator::ShortStartSequence || separator == Separator::StartSequence;
	auto detectLong =
	    separator == Separator::LongStartSequence || separator == Separator::StartSequence;
	switch (match) {
	case NUSM_noMatch:
		if (byte == 0x00) {
			return NUSM_firstZero;
		}
		break;
	case NUSM_firstZero:
		if (byte == 0x00) {
			return NUSM_secondZero;
		}
		break;
	case NUSM_secondZero:
		if (byte == 0x00 && detectLong) {
			return NUSM_thirdZero;
		} else if (byte == 0x00 && detectShort) {
			return NUSM_secondZero;
		} else if (byte == 0x01 && detectShort) {
			return NUSM_shortMatch;
		}
		break;
	case NUSM_thirdZero:
		if (byte == 0x00 && detectLong) {
			return NUSM_thirdZero;
		} else if (byte == 0x01 && detectLong) {
			return NUSM_longMatch;
		}
		break;
	case NUSM_shortMatch:
		return NUSM_shortMatch;
	case NUSM_longMatch:
		return NUSM_longMatch;
	}
	return NUSM_noMatch;
}

NalUnitFragmentA::NalUnitFragmentA(FragmentType type, bool forbiddenBit, uint8_t nri,
                                   uint8_t unitType, binary data)
    : NalUnit(data.size() + 2) {
	setForbiddenBit(forbiddenBit);
	setNRI(nri);
	fragmentIndicator()->setUnitType(NalUnitFragmentA::nal_type_fu_A);
	setFragmentType(type);
	setUnitType(unitType);
	copy(data.begin(), data.end(), begin() + 2);
}

// For backward compatibility, do not use
std::vector<shared_ptr<NalUnitFragmentA>>
NalUnitFragmentA::fragmentsFrom(shared_ptr<NalUnit> nalu, uint16_t maxFragmentSize) {
	auto fragments = nalu->generateFragments(maxFragmentSize);
	std::vector<shared_ptr<NalUnitFragmentA>> result;
	result.reserve(fragments.size());
	for (auto fragment : fragments)
		result.push_back(std::make_shared<NalUnitFragmentA>(std::move(fragment)));

	return result;
}

void NalUnitFragmentA::setFragmentType(FragmentType type) {
	fragmentHeader()->setReservedBit6(false);
	switch (type) {
	case FragmentType::Start:
		fragmentHeader()->setStart(true);
		fragmentHeader()->setEnd(false);
		break;
	case FragmentType::End:
		fragmentHeader()->setStart(false);
		fragmentHeader()->setEnd(true);
		break;
	default:
		fragmentHeader()->setStart(false);
		fragmentHeader()->setEnd(false);
	}
}

// For backward compatibility, do not use
std::vector<shared_ptr<binary>> NalUnits::generateFragments(uint16_t maxFragmentSize) {
	std::vector<NalUnit> nalus;
	for (auto nalu : *this)
		nalus.push_back(*nalu);

	auto fragments = NalUnit::GenerateFragments(nalus, maxFragmentSize);
	std::vector<shared_ptr<binary>> result;
	result.reserve(fragments.size());
	for (auto fragment : fragments)
		result.push_back(std::make_shared<binary>(std::move(fragment)));

	return result;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
