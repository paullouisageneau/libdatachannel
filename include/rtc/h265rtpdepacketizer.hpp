/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020-2024 Paul-Louis Ageneau
 * Copyright (c) 2024 Robert Edmonds
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_H265_RTP_DEPACKETIZER_H
#define RTC_H265_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "h265nalunit.hpp"
#include "message.hpp"
#include "rtp.hpp"
#include "rtpdepacketizer.hpp"

#include <set>

namespace rtc {

/// RTP depacketization for H265
class RTC_CPP_EXPORT H265RtpDepacketizer final : public VideoRtpDepacketizer {
public:
	using Separator = NalUnit::Separator;

	H265RtpDepacketizer(Separator separator = Separator::StartSequence);
	~H265RtpDepacketizer();

private:
	message_ptr reassemble(message_buffer &buffer);
	void addSeparator(binary &frame);

	const NalUnit::Separator mSeparator;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_H265_RTP_DEPACKETIZER_H
