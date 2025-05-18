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
#include "mediahandler.hpp"
#include "message.hpp"
#include "nalunit.hpp"
#include "rtp.hpp"

#include <set>

namespace rtc {

/// RTP depacketization for H264
class RTC_CPP_EXPORT H264RtpDepacketizer : public MediaHandler {
public:
	using Separator = NalUnit::Separator;

	inline static const uint32_t ClockRate = 90 * 1000;

	H264RtpDepacketizer(Separator separator = Separator::LongStartSequence);
	virtual ~H264RtpDepacketizer() = default;

	void incoming(message_vector &messages, const message_callback &send) override;

private:
	void addSeparator(binary &accessUnit);
	message_ptr buildFrame();

	const NalUnit::Separator mSeparator;

	struct sequence_cmp {
		bool operator() (message_ptr a, message_ptr b) const;
    };
	std::set<message_ptr, sequence_cmp> mBuffer;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_H264_RTP_DEPACKETIZER_H */
