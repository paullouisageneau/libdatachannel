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

#include "selectinterrupter.hpp"
#include "internals.hpp"

#if RTC_ENABLE_WEBSOCKET

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rtc::impl {

SelectInterrupter::SelectInterrupter() {
#ifndef _WIN32
	int pipefd[2];
	if (::pipe(pipefd) != 0)
		throw std::runtime_error("Failed to create pipe");
	::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	::fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
	mPipeOut = pipefd[1]; // read
	mPipeIn = pipefd[0];  // write
#endif
}

SelectInterrupter::~SelectInterrupter() {
	std::lock_guard lock(mMutex);
#ifdef _WIN32
	if (mDummySock != INVALID_SOCKET)
		::closesocket(mDummySock);
#else
	::close(mPipeIn);
	::close(mPipeOut);
#endif
}

int SelectInterrupter::prepare(fd_set &readfds) {
	std::lock_guard lock(mMutex);
#ifdef _WIN32
	if (mDummySock == INVALID_SOCKET)
		mDummySock = ::socket(AF_INET, SOCK_DGRAM, 0);
	FD_SET(mDummySock, &readfds);
	return SOCKET_TO_INT(mDummySock) + 1;
#else
	char dummy;
	if (::read(mPipeIn, &dummy, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		PLOG_WARNING << "Reading from interrupter pipe failed, errno=" << errno;
	}
	FD_SET(mPipeIn, &readfds);
	return mPipeIn + 1;
#endif
}

void SelectInterrupter::interrupt() {
	std::lock_guard lock(mMutex);
#ifdef _WIN32
	if (mDummySock != INVALID_SOCKET) {
		::closesocket(mDummySock);
		mDummySock = INVALID_SOCKET;
	}
#else
	char dummy = 0;
	if (::write(mPipeOut, &dummy, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		PLOG_WARNING << "Writing to interrupter pipe failed, errno=" << errno;
	}
#endif
}

} // namespace rtc::impl

#endif
