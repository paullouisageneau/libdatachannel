/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "opuspacketizationhandler.hpp"

namespace rtc {

OpusPacketizationHandler::OpusPacketizationHandler(shared_ptr<OpusRtpPacketizer> packetizer)
    : MediaChainableHandler(packetizer) {}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
