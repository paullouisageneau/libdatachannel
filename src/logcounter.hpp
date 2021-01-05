/**
 * Copyright (c) 2021 Staz Modrzynski
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef WEBRTC_SERVER_LOGCOUNTER_HPP
#define WEBRTC_SERVER_LOGCOUNTER_HPP

#include "threadpool.hpp"
#include "include.hpp"

namespace rtc {
class LogCounter: public std::enable_shared_from_this<LogCounter> {
private:
    plog::Severity severity;
    std::string text;
    std::chrono::steady_clock::duration duration;

    int count = 0;
    std::mutex mutex;
public:

    LogCounter(plog::Severity severity, const std::string& text, std::chrono::seconds duration=std::chrono::seconds(1));

    LogCounter& operator++(int);
};
}

#endif //WEBRTC_SERVER_LOGCOUNTER_HPP
