//
// Created by staz on 1/3/21.
//

#ifndef WEBRTC_SERVER_LOGCOUNTER_HPP
#define WEBRTC_SERVER_LOGCOUNTER_HPP

#include <plog/Util.h>
#include <plog/Severity.h>
#include "threadpool.hpp"
#include "include.hpp"

namespace rtc {
class LogCounter {
private:
    plog::Severity severity;
    std::string text;
    std::chrono::steady_clock::duration duration;

    int count = 0;
    std::mutex mutex;
    std::optional<invoke_future_t<void (*)()>> future;
public:

    LogCounter(plog::Severity severity, const std::string& text, std::chrono::seconds duration=std::chrono::seconds(1)):
        severity(severity), text(text), duration(duration) {}

    LogCounter& operator++(int) {
        std::lock_guard lock(mutex);
        count++;
        if (!future) {
            future = ThreadPool::Instance().schedule(duration, [this]() {
                int countCopy;
                {
                    std::lock_guard lock(mutex);
                    countCopy = count;
                    count = 0;
                    future = std::nullopt;
                }
                PLOG(severity) << text << ": " << countCopy << " (over " << std::chrono::duration_cast<std::chrono::seconds>(duration).count() << " seconds)";
            });
        }
        return *this;
    }
};
}

#endif //WEBRTC_SERVER_LOGCOUNTER_HPP
