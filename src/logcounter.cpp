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

#include "logcounter.hpp"

rtc::LogCounter::LogCounter(plog::Severity severity, const std::string &text, std::chrono::seconds duration) :
        severity(severity), text(text), duration(duration) {}

rtc::LogCounter& rtc::LogCounter::operator++(int) {
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

rtc::LogCounter::~LogCounter() {
    if (future) {
        future->wait();
    }
}
