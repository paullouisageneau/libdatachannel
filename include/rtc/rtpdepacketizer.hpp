/**
 * Copyright (c) 2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTP_DEPACKETIZER_H
#define RTC_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "message.hpp"

namespace rtc {

class RTC_CPP_EXPORT RtpDepacketizer : public MediaHandler {
public:
	RtpDepacketizer() = default;
	virtual ~RtpDepacketizer() = default;

	virtual void incoming(message_vector &messages, const message_callback &send) override;
};

using OpusRtpDepacketizer = RtpDepacketizer;
using AACRtpDepacketizer = RtpDepacketizer;

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTP_DEPACKETIZER_H */
