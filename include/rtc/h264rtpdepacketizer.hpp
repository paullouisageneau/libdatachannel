/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_H264_RTP_DEPACKETIZER_H
#define RTC_H264_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "message.hpp"
#include "nalunit.hpp"
#include "rtp.hpp"
#include "rtpdepacketizer.hpp"

namespace rtc {

/// RTP depacketization for H264
class RTC_CPP_EXPORT H264RtpDepacketizer final : public VideoRtpDepacketizer {
public:
	using Separator = NalUnit::Separator;

	H264RtpDepacketizer(Separator separator = Separator::StartSequence);
	~H264RtpDepacketizer();

private:
	message_ptr reassemble(message_buffer &buffer) override;
	void addSeparator(binary &frame);

	const NalUnit::Separator mSeparator;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_H264_RTP_DEPACKETIZER_H */
