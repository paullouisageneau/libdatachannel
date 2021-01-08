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

#ifndef h264fileparser_hpp
#define h264fileparser_hpp

#include "fileparser.hpp"
#include <optional>

class H264FileParser: public FileParser {
    std::optional<std::vector<std::byte>> previousUnitType5 = std::nullopt;
    std::optional<std::vector<std::byte>> previousUnitType7 = std::nullopt;
    std::optional<std::vector<std::byte>> previousUnitType8 = std::nullopt;

public:
    H264FileParser(std::string directory, uint32_t fps, bool loop);
    void loadNextSample() override;
    std::vector<std::byte> initialNALUS();
};

#endif /* h264fileparser_hpp */
