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

std::vector<shared_ptr<NalUnitFragmentA>>
NalUnitFragmentA::fragmentsFrom(shared_ptr<NalUnit> nalu, uint16_t maxFragmentSize) {
	assert(nalu->size() > maxFragmentSize);
	auto fragments_count = ceil(double(nalu->size()) / maxFragmentSize);
	maxFragmentSize = uint16_t(int(ceil(nalu->size() / fragments_count)));

	// 2 bytes for FU indicator and FU header
	maxFragmentSize -= 2;
	auto f = nalu->forbiddenBit();
	uint8_t nri = nalu->nri() & 0x03;
	uint8_t naluType = nalu->unitType() & 0x1F;
	auto payload = nalu->payload();
	vector<shared_ptr<NalUnitFragmentA>> result{};
	uint64_t offset = 0;
	while (offset < payload.size()) {
		vector<byte> fragmentData;
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
		auto fragment =
		    std::make_shared<NalUnitFragmentA>(fragmentType, f, nri, naluType, fragmentData);
		result.push_back(fragment);
		offset += maxFragmentSize;
	}
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

std::vector<shared_ptr<binary>> NalUnits::generateFragments(uint16_t maxFragmentSize) {
	vector<shared_ptr<binary>> result{};
	for (auto nalu : *this) {
		if (nalu->size() > maxFragmentSize) {
			std::vector<shared_ptr<NalUnitFragmentA>> fragments =
			    NalUnitFragmentA::fragmentsFrom(nalu, maxFragmentSize);
			result.insert(result.end(), fragments.begin(), fragments.end());
		} else {
			result.push_back(nalu);
		}
	}
	return result;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
