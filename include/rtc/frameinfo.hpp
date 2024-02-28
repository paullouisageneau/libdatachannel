/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_FRAMEINFO_H
#define RTC_FRAMEINFO_H

#include "common.hpp"

namespace rtc {

struct RTC_CPP_EXPORT FrameInfo {
	FrameInfo(uint32_t timestamp) : timestamp(timestamp){};
	uint32_t timestamp = 0; // RTP Timestamp
};

} // namespace rtc

#endif // RTC_FRAMEINFO_H
