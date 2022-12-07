/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "stream.hpp"
#include "helpers.hpp"

#ifdef _WIN32
// taken from https://stackoverflow.com/questions/5801813/c-usleep-is-obsolete-workarounds-for-windows-mingw
#include <windows.h>

void usleep(__int64 usec)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    ft.QuadPart = -(10*usec); // Convert to 100 nanosecond interval, negative value indicates relative time

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}
#else
#include <unistd.h>
#endif

Stream::Stream(std::shared_ptr<StreamSource> video, std::shared_ptr<StreamSource> audio):
	std::enable_shared_from_this<Stream>(), video(video), audio(audio) { }

Stream::~Stream() {
    stop();
}

std::pair<std::shared_ptr<StreamSource>, Stream::StreamSourceType> Stream::unsafePrepareForSample() {
    std::shared_ptr<StreamSource> ss;
    StreamSourceType sst;
    uint64_t nextTime;
    if (audio->getSampleTime_us() < video->getSampleTime_us()) {
        ss = audio;
        sst = StreamSourceType::Audio;
        nextTime = audio->getSampleTime_us();
    } else {
        ss = video;
        sst = StreamSourceType::Video;
        nextTime = video->getSampleTime_us();
    }

    auto currentTime = currentTimeInMicroSeconds();

    auto elapsed = currentTime - startTime;
    if (nextTime > elapsed) {
        auto waitTime = nextTime - elapsed;
        mutex.unlock();
        usleep(waitTime);
        mutex.lock();
    }
    return {ss, sst};
}

void Stream::sendSample() {
    std::lock_guard lock(mutex);
    if (!isRunning) {
        return;
    }
    auto ssSST = unsafePrepareForSample();
    auto ss = ssSST.first;
    auto sst = ssSST.second;
    auto sample = ss->getSample();
    sampleHandler(sst, ss->getSampleTime_us(), sample);
    ss->loadNextSample();
    dispatchQueue.dispatch([this]() {
        this->sendSample();
    });
}

void Stream::onSample(std::function<void (StreamSourceType, uint64_t, rtc::binary)> handler) {
    sampleHandler = handler;
}

void Stream::start() {
    std::lock_guard lock(mutex);
    if (isRunning) {
        return;
    }
    _isRunning = true;
    startTime = currentTimeInMicroSeconds();
    audio->start();
    video->start();
    dispatchQueue.dispatch([this]() {
        this->sendSample();
    });
}

void Stream::stop() {
    std::lock_guard lock(mutex);
    if (!isRunning) {
        return;
    }
    _isRunning = false;
    dispatchQueue.removePending();
    audio->stop();
    video->stop();
};

