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

#ifndef RTC_SERVER_LOGCOUNTER_HPP
#define RTC_SERVER_LOGCOUNTER_HPP

#include "common.hpp"
#include "threadpool.hpp"

#include <atomic>
#include <chrono>

namespace rtc::impl {

class LogCounter {
private:
	struct LogData {
		plog::Severity mSeverity;
		std::string mText;
		std::chrono::steady_clock::duration mDuration;

		std::atomic<int> mCount = 0;
	};

	shared_ptr<LogData> mData;

public:
	LogCounter(plog::Severity severity, const std::string &text,
	           std::chrono::seconds duration = std::chrono::seconds(1));

	LogCounter &operator++(int);
};

} // namespace rtc

#endif // RTC_SERVER_LOGCOUNTER_HPP
