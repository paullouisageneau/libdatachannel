/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
