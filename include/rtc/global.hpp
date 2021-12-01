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

#ifndef RTC_GLOBAL_H
#define RTC_GLOBAL_H

#include "common.hpp"

#include <chrono>
#include <iostream>
#include <future>

namespace rtc {

enum class LogLevel { // Don't change, it must match plog severity
	None = 0,
	Fatal = 1,
	Error = 2,
	Warning = 3,
	Info = 4,
	Debug = 5,
	Verbose = 6
};

typedef std::function<void(LogLevel level, string message)> LogCallback;

RTC_CPP_EXPORT void InitLogger(LogLevel level, LogCallback callback = nullptr);

#ifdef PLOG_DEFAULT_INSTANCE_ID
// Deprecated, kept for retro-compatibility
[[deprecated]]
RTC_CPP_EXPORT void InitLogger(plog::Severity severity, plog::IAppender *appender = nullptr);
#endif

RTC_CPP_EXPORT void Preload();
RTC_CPP_EXPORT std::shared_future<void> Cleanup();

struct SctpSettings {
	// For the following settings, not set means optimized default
	optional<size_t> recvBufferSize;                // in bytes
	optional<size_t> sendBufferSize;                // in bytes
	optional<size_t> maxChunksOnQueue;              // in chunks
	optional<size_t> initialCongestionWindow;       // in MTUs
	optional<size_t> maxBurst;                      // in MTUs
	optional<unsigned int> congestionControlModule; // 0: RFC2581, 1: HSTCP, 2: H-TCP, 3: RTCC
	optional<std::chrono::milliseconds> delayedSackTime;
	optional<std::chrono::milliseconds> minRetransmitTimeout;
	optional<std::chrono::milliseconds> maxRetransmitTimeout;
	optional<std::chrono::milliseconds> initialRetransmitTimeout;
	optional<unsigned int> maxRetransmitAttempts;
	optional<std::chrono::milliseconds> heartbeatInterval;
};

RTC_CPP_EXPORT void SetSctpSettings(SctpSettings s);

} // namespace rtc

RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, rtc::LogLevel level);

#endif
