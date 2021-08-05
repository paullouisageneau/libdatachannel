/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
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

	bool forbiddenBit() { return _first >> 7; }
	uint8_t nri() { return _first >> 5 & 0x03; }
	uint8_t unitType() { return _first & 0x1F; }

	void setForbiddenBit(bool isSet) { _first = (_first & 0x7F) | (isSet << 7); }
	void setNRI(uint8_t nri) { _first = (_first & 0x9F) | ((nri & 0x03) << 5); }
	void setUnitType(uint8_t type) { _first = (_first & 0xE0) | (type & 0x1F); }
};

/// Nalu fragment header
struct RTC_CPP_EXPORT NalUnitFragmentHeader {
	uint8_t _first = 0;

	bool isStart() { return _first >> 7; }
	bool reservedBit6() { return (_first >> 6) & 0x01; }
	bool isEnd() { return (_first >> 5) & 0x01; }
	uint8_t unitType() { return _first & 0x1F; }

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

	template <typename Iterator> NalUnit(Iterator begin_, Iterator end_) : binary(begin_, end_) {}

	NalUnit(binary &&data) : binary(std::move(data)) {}

	NalUnit() : binary(1) {}

	bool forbiddenBit() { return header()->forbiddenBit(); }
	uint8_t nri() { return header()->nri(); }
	uint8_t unitType() { return header()->unitType(); }
	binary payload() {
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
	NalUnitHeader *header() {
		assert(size() >= 1);
		return (NalUnitHeader *)data();
	}
};

/// Nal unit fragment A
struct RTC_CPP_EXPORT NalUnitFragmentA : NalUnit {
	enum class FragmentType { Start, Middle, End };

	NalUnitFragmentA(FragmentType type, bool forbiddenBit, uint8_t nri, uint8_t unitType,
	                 binary data);

	static std::vector<shared_ptr<NalUnitFragmentA>>
	fragmentsFrom(shared_ptr<NalUnit> nalu, uint16_t maximumFragmentSize);

	uint8_t unitType() { return fragmentHeader()->unitType(); }

	binary payload() {
		assert(size() >= 2);
		return {begin() + 2, end()};
	}

	FragmentType type() {
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
	NalUnitHeader *fragmentIndicator() { return (NalUnitHeader *)data(); }

	NalUnitFragmentHeader *fragmentHeader() {
		return (NalUnitFragmentHeader *)fragmentIndicator() + 1;
	}

	const uint8_t nal_type_fu_A = 28;
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
