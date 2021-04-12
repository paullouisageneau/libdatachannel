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

namespace rtc {

RTC_EXPORT void Preload();
RTC_EXPORT void Cleanup();

struct SctpSettings {
	optional<size_t> recvBufferSize;
	optional<size_t> sendBufferSize;
	optional<size_t> maxChunksOnQueue;
	optional<size_t> initialCongestionWindow;
	optional<unsigned int> congestionControlModule;
	optional<std::chrono::milliseconds> delayedSackTime;
};

RTC_EXPORT void SetSctpSettings(SctpSettings s);

} // namespace rtc

#endif
