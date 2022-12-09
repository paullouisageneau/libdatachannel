/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef fileparser_hpp
#define fileparser_hpp

#include <string>
#include <vector>
#include "stream.hpp"

class FileParser: public StreamSource {
    std::string directory;
    std::string extension;
    uint64_t sampleDuration_us;
    uint64_t sampleTime_us = 0;
    uint32_t counter = -1;
    bool loop;
    uint64_t loopTimestampOffset = 0;
protected:
    rtc::binary sample = {};
public:
    FileParser(std::string directory, std::string extension, uint32_t samplesPerSecond, bool loop);
    virtual ~FileParser();
    virtual void start() override;
    virtual void stop() override;
    virtual void loadNextSample() override;

    rtc::binary getSample() override;
    uint64_t getSampleTime_us() override;
    uint64_t getSampleDuration_us() override;
};

#endif /* fileparser_hpp */
