/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_NAL_UNIT_H
#define RTC_NAL_UNIT_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"

#include <cassert>

namespace rtc {

#pragma pack(push, 1)

/// Nalu header
struct RTC_CPP_EXPORT NalUnitHeader {
	uint8_t _first = 0;

	bool forbiddenBit() const { return _first >> 7; }
	uint8_t nri() const { return _first >> 5 & 0x03; }
	uint8_t unitType() const { return _first & 0x1F; }

	void setForbiddenBit(bool isSet) { _first = (_first & 0x7F) | (isSet << 7); }
	void setNRI(uint8_t nri) { _first = (_first & 0x9F) | ((nri & 0x03) << 5); }
	void setUnitType(uint8_t type) { _first = (_first & 0xE0) | (type & 0x1F); }
};

/// Nalu fragment header
struct RTC_CPP_EXPORT NalUnitFragmentHeader {
	uint8_t _first = 0;

	bool isStart() const { return _first >> 7; }
	bool reservedBit6() const { return (_first >> 5) & 0x01; }
	bool isEnd() const { return (_first >> 6) & 0x01; }
	uint8_t unitType() const { return _first & 0x1F; }

	void setStart(bool isSet) { _first = (_first & 0x7F) | (isSet << 7); }
	void setEnd(bool isSet) { _first = (_first & 0xBF) | (isSet << 6); }
	void setReservedBit6(bool isSet) { _first = (_first & 0xDF) | (isSet << 5); }
	void setUnitType(uint8_t type) { _first = (_first & 0xE0) | (type & 0x1F); }
};

#pragma pack(pop)

/// Nal unit
struct RTC_CPP_EXPORT NalUnit : binary {
	NalUnit(const NalUnit &unit) = default;
	NalUnit(size_t size, bool includingHeader = true) : binary(size + (includingHeader ? 0 : 1)) {}
	NalUnit(binary &&data) : binary(std::move(data)) {}
	NalUnit() : binary(1) {}
	template <typename Iterator> NalUnit(Iterator begin_, Iterator end_) : binary(begin_, end_) {}

	bool forbiddenBit() const { return header()->forbiddenBit(); }
	uint8_t nri() const { return header()->nri(); }
	uint8_t unitType() const { return header()->unitType(); }

	binary payload() const {
		assert(size() >= 1);
		return {begin() + 1, end()};
	}

	void setForbiddenBit(bool isSet) { header()->setForbiddenBit(isSet); }
	void setNRI(uint8_t nri) { header()->setNRI(nri); }
	void setUnitType(uint8_t type) { header()->setUnitType(type); }

	void setPayload(binary payload) {
		assert(size() >= 1);
		erase(begin() + 1, end());
		insert(end(), payload.begin(), payload.end());
	}

protected:
	const NalUnitHeader *header() const {
		assert(size() >= 1);
		return reinterpret_cast<const NalUnitHeader *>(data());
	}

	NalUnitHeader *header() {
		assert(size() >= 1);
		return reinterpret_cast<NalUnitHeader *>(data());
	}
};

/// Nal unit fragment A
struct RTC_CPP_EXPORT NalUnitFragmentA : NalUnit {
	static std::vector<shared_ptr<NalUnitFragmentA>> fragmentsFrom(shared_ptr<NalUnit> nalu,
	                                                               uint16_t maximumFragmentSize);

	enum class FragmentType { Start, Middle, End };

	NalUnitFragmentA(FragmentType type, bool forbiddenBit, uint8_t nri, uint8_t unitType,
	                 binary data);

	uint8_t unitType() const { return fragmentHeader()->unitType(); }

	binary payload() const {
		assert(size() >= 2);
		return {begin() + 2, end()};
	}

	FragmentType type() const {
		if (fragmentHeader()->isStart()) {
			return FragmentType::Start;
		} else if (fragmentHeader()->isEnd()) {
			return FragmentType::End;
		} else {
			return FragmentType::Middle;
		}
	}

	void setUnitType(uint8_t type) { fragmentHeader()->setUnitType(type); }

	void setPayload(binary payload) {
		assert(size() >= 2);
		erase(begin() + 2, end());
		insert(end(), payload.begin(), payload.end());
	}

	void setFragmentType(FragmentType type);

protected:
	const uint8_t nal_type_fu_A = 28;

	NalUnitHeader *fragmentIndicator() { return reinterpret_cast<NalUnitHeader *>(data()); }

	const NalUnitHeader *fragmentIndicator() const {
		return reinterpret_cast<const NalUnitHeader *>(data());
	}

	NalUnitFragmentHeader *fragmentHeader() {
		return reinterpret_cast<NalUnitFragmentHeader *>(fragmentIndicator() + 1);
	}

	const NalUnitFragmentHeader *fragmentHeader() const {
		return reinterpret_cast<const NalUnitFragmentHeader *>(fragmentIndicator() + 1);
	}
};

class RTC_CPP_EXPORT NalUnits : public std::vector<shared_ptr<NalUnit>> {
public:
	static const uint16_t defaultMaximumFragmentSize =
	    uint16_t(RTC_DEFAULT_MTU - 12 - 8 - 40); // SRTP/UDP/IPv6

	std::vector<shared_ptr<binary>> generateFragments(uint16_t maximumFragmentSize);
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_NAL_UNIT_H */
