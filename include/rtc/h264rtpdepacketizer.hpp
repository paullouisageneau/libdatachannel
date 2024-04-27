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

#include <iterator>

namespace rtc {

/// RTP depacketization for H264
class RTC_CPP_EXPORT H264RtpDepacketizer : public MediaHandler {
public:
	using Separator = NalUnit::Separator;

	H264RtpDepacketizer(Separator separator = Separator::LongStartSequence);
	virtual ~H264RtpDepacketizer() = default;

	void incoming(message_vector &messages, const message_callback &send) override;

private:
	std::vector<message_ptr> mRtpBuffer;
	const NalUnit::Separator mSeparator;

	void addSeparator(binary &accessUnit);
	message_vector buildFrames(message_vector::iterator firstPkt, message_vector::iterator lastPkt,
	                           uint8_t payloadType, uint32_t timestamp);
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_H264_RTP_DEPACKETIZER_H */
