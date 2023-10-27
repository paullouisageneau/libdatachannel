/**
 * Copyright (c) 2023 Arda Cinar
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_PLI_RESPONDER_H
#define RTC_PLI_RESPONDER_H

#if RTC_ENABLE_MEDIA

#include "mediahandlerelement.hpp"
#include "utils.hpp"
#include <functional>

namespace rtc {

/// Responds to PLI and FIR messages sent by the receiver. The sender should respond to these
/// messages by sending an intra. 
class RTC_CPP_EXPORT PliHandler final : public MediaHandlerElement {
    rtc::synchronized_callback<> mOnPli;

public:
	/// Constructs the PLIResponder object to notify whenever a new intra frame is requested
	/// @param onPli The callback that gets called whenever an intra frame is requested by the receiver
    PliHandler(std::function<void(void)> onPli);
    ChainedIncomingControlProduct processIncomingControlMessage(message_ptr) override;
};

}

#endif // RTC_ENABLE_MEDIA

#endif // RTC_PLI_RESPONDER_H
