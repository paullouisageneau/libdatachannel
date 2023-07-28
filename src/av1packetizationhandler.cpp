/**
 * Copyright (c) 2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "av1packetizationhandler.hpp"

namespace rtc {

AV1PacketizationHandler::AV1PacketizationHandler(shared_ptr<AV1RtpPacketizer> packetizer)
    : MediaChainableHandler(packetizer) {}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
