/*
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include "h264fileparser.hpp"
#include "rtc/rtc.hpp"

#include <fstream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace std;

H264FileParser::H264FileParser(string directory, uint32_t fps, bool loop): FileParser(directory, ".h264", fps, loop) { }

void H264FileParser::loadNextSample() {
    FileParser::loadNextSample();

    size_t i = 0;
    while (i < sample.size()) {
        assert(i + 4 < sample.size());
        auto lengthPtr = (uint32_t *) (sample.data() + i);
        uint32_t length = ntohl(*lengthPtr);
        auto naluStartIndex = i + 4;
        auto naluEndIndex = naluStartIndex + length;
        assert(naluEndIndex <= sample.size());
        auto header = reinterpret_cast<rtc::NalUnitHeader *>(sample.data() + naluStartIndex);
        auto type = header->unitType();
        switch (type) {
            case 7:
                previousUnitType7 = {sample.begin() + i, sample.begin() + naluEndIndex};
                break;
            case 8:
                previousUnitType8 = {sample.begin() + i, sample.begin() + naluEndIndex};;
                break;
            case 5:
                previousUnitType5 = {sample.begin() + i, sample.begin() + naluEndIndex};;
                break;
        }
        i = naluEndIndex;
    }
}

vector<byte> H264FileParser::initialNALUS() {
    vector<byte> units{};
    if (previousUnitType7.has_value()) {
        auto nalu = previousUnitType7.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    if (previousUnitType8.has_value()) {
        auto nalu = previousUnitType8.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    if (previousUnitType5.has_value()) {
        auto nalu = previousUnitType5.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    return units;
}
