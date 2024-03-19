/**
 * Copyright (c) 2023 Zita Liao (Dolby)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "h265nalunit.hpp"

#include "impl/internals.hpp"

#include <cmath>

namespace rtc {

H265NalUnitFragment::H265NalUnitFragment(FragmentType type, bool forbiddenBit, uint8_t nuhLayerId,
                                         uint8_t nuhTempIdPlus1, uint8_t unitType, binary data)
    : H265NalUnit(data.size() + H265_NAL_HEADER_SIZE + H265_FU_HEADER_SIZE) {
	setForbiddenBit(forbiddenBit);
	setNuhLayerId(nuhLayerId);
	setNuhTempIdPlus1(nuhTempIdPlus1);
	fragmentIndicator()->setUnitType(H265NalUnitFragment::nal_type_fu);
	setFragmentType(type);
	setUnitType(unitType);
	copy(data.begin(), data.end(), begin() + H265_NAL_HEADER_SIZE + H265_FU_HEADER_SIZE);
}

std::vector<shared_ptr<H265NalUnitFragment>>
H265NalUnitFragment::fragmentsFrom(shared_ptr<H265NalUnit> nalu, uint16_t maxFragmentSize) {
	assert(nalu->size() > maxFragmentSize);
	auto fragments_count = ceil(double(nalu->size()) / maxFragmentSize);
	maxFragmentSize = uint16_t(int(ceil(nalu->size() / fragments_count)));

	// 3 bytes for NALU header and FU header
	maxFragmentSize -= (H265_NAL_HEADER_SIZE + H265_FU_HEADER_SIZE);
	auto f = nalu->forbiddenBit();
	uint8_t nuhLayerId = nalu->nuhLayerId() & 0x3F;        // 6 bits
	uint8_t nuhTempIdPlus1 = nalu->nuhTempIdPlus1() & 0x7; // 3 bits
	uint8_t naluType = nalu->unitType() & 0x3F;            // 6 bits
	auto payload = nalu->payload();
	vector<shared_ptr<H265NalUnitFragment>> result{};
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
		auto fragment = std::make_shared<H265NalUnitFragment>(
		    fragmentType, f, nuhLayerId, nuhTempIdPlus1, naluType, fragmentData);
		result.push_back(fragment);
		offset += maxFragmentSize;
	}
	return result;
}

void H265NalUnitFragment::setFragmentType(FragmentType type) {
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

std::vector<shared_ptr<binary>> H265NalUnits::generateFragments(uint16_t maxFragmentSize) {
	vector<shared_ptr<binary>> result{};
	for (auto nalu : *this) {
		if (nalu->size() > maxFragmentSize) {
			std::vector<shared_ptr<H265NalUnitFragment>> fragments =
			    H265NalUnitFragment::fragmentsFrom(nalu, maxFragmentSize);
			result.insert(result.end(), fragments.begin(), fragments.end());
		} else {
			result.push_back(nalu);
		}
	}
	return result;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
