/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "helpers.hpp"

#include <ctime>

#ifdef _MSC_VER
// taken from https://stackoverflow.com/questions/10905892/equivalent-of-gettimeday-for-windows
#include <windows.h>
#include <winsock2.h> // for struct timeval

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz) {
	if (tv) {
		FILETIME filetime; /* 64-bit value representing the number of 100-nanosecond intervals since
		                      January 1, 1601 00:00 UTC */
		ULARGE_INTEGER x;
		ULONGLONG usec;
		static const ULONGLONG epoch_offset_us =
		    11644473600000000ULL; /* microseconds betweeen Jan 1,1601 and Jan 1,1970 */

#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
		GetSystemTimePreciseAsFileTime(&filetime);
#else
		GetSystemTimeAsFileTime(&filetime);
#endif
		x.LowPart = filetime.dwLowDateTime;
		x.HighPart = filetime.dwHighDateTime;
		usec = x.QuadPart / 10 - epoch_offset_us;
		tv->tv_sec = time_t(usec / 1000000ULL);
		tv->tv_usec = long(usec % 1000000ULL);
	}
	if (tz) {
		TIME_ZONE_INFORMATION timezone;
		GetTimeZoneInformation(&timezone);
		tz->tz_minuteswest = timezone.Bias;
		tz->tz_dsttime = 0;
	}
	return 0;
}
#else
#include <sys/time.h>
#endif

using namespace std;
using namespace rtc;

ClientTrackData::ClientTrackData(shared_ptr<Track> track, shared_ptr<RtcpSrReporter> sender) {
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
