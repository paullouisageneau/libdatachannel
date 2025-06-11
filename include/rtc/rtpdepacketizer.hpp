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

#include <set>

namespace rtc {

// Base RTP depacketizer class
class RTC_CPP_EXPORT RtpDepacketizer : public MediaHandler {
public:
	RtpDepacketizer();
	RtpDepacketizer(uint32_t clockRate);
	virtual ~RtpDepacketizer();

	virtual void incoming(message_vector &messages, const message_callback &send) override;

protected:
	shared_ptr<FrameInfo> createFrameInfo(uint32_t timestamp, uint8_t payloadType) const;

private:
	const uint32_t mClockRate;
};

// Base class for video RTP depacketizer
class RTC_CPP_EXPORT VideoRtpDepacketizer : public RtpDepacketizer {
public:
	inline static const uint32_t ClockRate = 90000;

	VideoRtpDepacketizer();
	virtual ~VideoRtpDepacketizer();

protected:
	struct sequence_cmp {
		bool operator()(message_ptr a, message_ptr b) const;
	};
	using message_buffer = std::set<message_ptr, sequence_cmp>;

	virtual message_ptr reassemble(message_buffer &messages) = 0;

private:
	void incoming(message_vector &messages, const message_callback &send) override;

	message_buffer mBuffer;
};

// Generic audio RTP depacketizer
template <uint32_t DEFAULT_CLOCK_RATE>
class RTC_CPP_EXPORT AudioRtpDepacketizer final : public RtpDepacketizer {
public:
	inline static const uint32_t DefaultClockRate = DEFAULT_CLOCK_RATE;

	AudioRtpDepacketizer(uint32_t clockRate = DefaultClockRate) : RtpDepacketizer(clockRate) {}
};

// Audio RTP depacketizers
using OpusRtpDepacketizer = AudioRtpDepacketizer<48000>;
using AACRtpDepacketizer = AudioRtpDepacketizer<48000>;
using PCMARtpDepacketizer = AudioRtpDepacketizer<8000>;
using PCMURtpDepacketizer = AudioRtpDepacketizer<8000>;
using G722RtpDepacketizer = AudioRtpDepacketizer<8000>;

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTP_DEPACKETIZER_H */
