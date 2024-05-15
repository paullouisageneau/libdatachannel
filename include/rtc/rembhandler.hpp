/**
 * Copyright (c) 2024 Vladimir Voronin
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_REMB_RESPONDER_H
#define RTC_REMB_RESPONDER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "utils.hpp"

namespace rtc {

/// Responds to REMB messages sent by the receiver.
class RTC_CPP_EXPORT RembHandler final : public MediaHandler {
    rtc::synchronized_callback<unsigned int> mOnRemb;

public:
	/// Constructs the RembResponder object to notify whenever a bitrate
	/// @param onRemb The callback that gets called whenever a bitrate by the receiver
    RembHandler(std::function<void(unsigned int)> onRemb);

	void incoming(message_vector &messages, const message_callback &send) override;
};

}

#endif // RTC_ENABLE_MEDIA

#endif // RTC_REMB_RESPONDER_H
