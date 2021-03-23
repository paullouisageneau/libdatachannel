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

#include "fileparser.hpp"
#include <fstream>

using namespace std;

FileParser::FileParser(string directory, string extension, uint32_t samplesPerSecond, bool loop): sampleDuration_us(1000 * 1000 / samplesPerSecond), StreamSource() {
    this->directory = directory;
    this->extension = extension;
    this->loop = loop;
}

void FileParser::start() {
    sampleTime_us = std::numeric_limits<uint64_t>::max() - sampleDuration_us + 1;
    loadNextSample();
}

void FileParser::stop() {
    StreamSource::stop();
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

    vector<uint8_t> fileContents((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    sample = *reinterpret_cast<vector<byte> *>(&fileContents);
    sampleTime_us += sampleDuration_us;
}
