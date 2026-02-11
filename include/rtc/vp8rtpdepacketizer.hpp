/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_VP8_RTP_DEPACKETIZER_H
#define RTC_VP8_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "message.hpp"
#include "rtpdepacketizer.hpp"

namespace rtc {

/// RTP depacketization for VP8
class RTC_CPP_EXPORT VP8RtpDepacketizer final : public VideoRtpDepacketizer {
public:
	VP8RtpDepacketizer();
	~VP8RtpDepacketizer();

private:
	message_ptr reassemble(message_buffer &buffer) override;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_VP8_RTP_DEPACKETIZER_H */
