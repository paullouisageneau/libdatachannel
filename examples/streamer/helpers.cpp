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

#include "helpers.hpp"
#include <sys/time.h>

using namespace std;
using namespace rtc;

ClientTrackData::ClientTrackData(shared_ptr<Track> track, shared_ptr<RTCPSenderReportable> sender) {
    this->track = track;
    this->sender = sender;
}

void Client::setState(State state) {
    std::unique_lock lock(_mutex);
    this->state = state;
}

Client::State Client::getState() {
    std::shared_lock lock(_mutex);
    return state;
}

ClientTrack::ClientTrack(string id, shared_ptr<ClientTrackData> trackData) {
    this->id = id;
    this->trackData = trackData;
}

uint64_t currentTimeInMicroSeconds() {
    struct timeval time;
    gettimeofday(&time, NULL);
    return uint64_t(time.tv_sec) * 1000 * 1000 + time.tv_usec;
}
