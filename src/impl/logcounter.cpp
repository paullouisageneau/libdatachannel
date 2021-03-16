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

namespace rtc::impl {

LogCounter::LogCounter(plog::Severity severity, const std::string &text,
                       std::chrono::seconds duration) {
	mData = std::make_shared<LogData>();
	mData->mDuration = duration;
	mData->mSeverity = severity;
	mData->mText = text;
}

LogCounter &LogCounter::operator++(int) {
	if (mData->mCount++ == 0) {
		ThreadPool::Instance().schedule(
		    mData->mDuration,
		    [](weak_ptr<LogData> data) {
			    if (auto ptr = data.lock()) {
				    int countCopy;
				    countCopy = ptr->mCount.exchange(0);
				    PLOG(ptr->mSeverity)
				        << ptr->mText << ": " << countCopy << " (over "
				        << std::chrono::duration_cast<std::chrono::seconds>(ptr->mDuration).count()
				        << " seconds)";
			    }
		    },
		    mData);
	}
	return *this;
}

} // namespace rtc
