/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
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

#ifndef RTC_IMPL_SELECT_INTERRUPTER_H
#define RTC_IMPL_SELECT_INTERRUPTER_H

#include "common.hpp"
#include "socket.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <mutex>

namespace rtc::impl {

// Utility class to interrupt select()
class SelectInterrupter final {
public:
	SelectInterrupter();
	~SelectInterrupter();

	int prepare(fd_set &readfds);
	void interrupt();

private:
	std::mutex mMutex;
#ifdef _WIN32
	socket_t mDummySock = INVALID_SOCKET;
#else // assume POSIX
	int mPipeIn, mPipeOut;
#endif
};

} // namespace rtc::impl

#endif

#endif
