/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef stream_hpp
#define stream_hpp

#include "dispatchqueue.hpp"
#include "rtc/rtc.hpp"

class StreamSource {
protected:

public:
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void loadNextSample() = 0;

    virtual uint64_t getSampleTime_us() = 0;
    virtual uint64_t getSampleDuration_us() = 0;
	virtual rtc::binary getSample() = 0;
};

class Stream: public std::enable_shared_from_this<Stream> {
    uint64_t startTime = 0;
    std::mutex mutex;
    DispatchQueue dispatchQueue = DispatchQueue("StreamQueue");

    bool _isRunning = false;
public:
    const std::shared_ptr<StreamSource> audio;
    const std::shared_ptr<StreamSource> video;

    Stream(std::shared_ptr<StreamSource> video, std::shared_ptr<StreamSource> audio);
    ~Stream();

    enum class StreamSourceType {
        Audio,
        Video
    };

private:
    rtc::synchronized_callback<StreamSourceType, uint64_t, rtc::binary> sampleHandler;

    std::pair<std::shared_ptr<StreamSource>, StreamSourceType> unsafePrepareForSample();

    void sendSample();

public:
    void onSample(std::function<void (StreamSourceType, uint64_t, rtc::binary)> handler);
    void start();
    void stop();
    const bool & isRunning = _isRunning;
};


#endif /* stream_hpp */
