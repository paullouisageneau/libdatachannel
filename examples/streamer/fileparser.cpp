/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "fileparser.hpp"
#include <fstream>

using namespace std;

FileParser::FileParser(string directory, string extension, uint32_t samplesPerSecond, bool loop) {
    this->directory = directory;
    this->extension = extension;
    this->loop = loop;
    this->sampleDuration_us = 1000 * 1000 / samplesPerSecond;
}

FileParser::~FileParser() {
	stop();
}

void FileParser::start() {
    sampleTime_us = std::numeric_limits<uint64_t>::max() - sampleDuration_us + 1;
    loadNextSample();
}

void FileParser::stop() {
    sample = {};
    sampleTime_us = 0;
    counter = -1;
}

void FileParser::loadNextSample() {
    string frame_id = to_string(++counter);

    string url = directory + "/sample-" + frame_id + extension;
    ifstream source(url, ios_base::binary);
    if (!source) {
        if (loop && counter > 0) {
            loopTimestampOffset = sampleTime_us;
            counter = -1;
            loadNextSample();
            return;
        }
        sample = {};
        return;
    }

    vector<char> contents((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    auto *b = reinterpret_cast<const std::byte*>(contents.data());
    sample.assign(b, b + contents.size());
    sampleTime_us += sampleDuration_us;
}

rtc::binary FileParser::getSample() {
	return sample;
}

uint64_t FileParser::getSampleTime_us() {
	return sampleTime_us;
}

uint64_t FileParser::getSampleDuration_us() {
	return sampleDuration_us;
}

