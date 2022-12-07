/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef opusfileparser_hpp
#define opusfileparser_hpp

#include "fileparser.hpp"

class OPUSFileParser: public FileParser {
    static const uint32_t defaultSamplesPerSecond = 50;

public:
    OPUSFileParser(std::string directory, bool loop, uint32_t samplesPerSecond = OPUSFileParser::defaultSamplesPerSecond);
};


#endif /* opusfileparser_hpp */
