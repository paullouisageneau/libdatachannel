/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTP_PACKETIZER_H
#define RTC_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "message.hpp"
#include "rtppacketizationconfig.hpp"

namespace rtc {

/// RTP packetizer
class RTC_CPP_EXPORT RtpPacketizer : public MediaHandler {
public:
	/// Constructs packetizer with given RTP configuration
	/// @note RTP configuration is used in packetization process which may change some configuration
	/// properties such as sequence number.
	/// @param rtpConfig  RTP configuration
	RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig);
	virtual ~RtpPacketizer();

	virtual void media(const Description::Media &desc) override;
	virtual void outgoing(message_vector &messages, const message_callback &send) override;

	/// RTP packetization config
	const shared_ptr<RtpPacketizationConfig> rtpConfig;

protected:
	/// Creates RTP packet for given payload
	/// @note This function increase sequence number after packetization.
	/// @param payload RTP payload
	/// @param setMark Set marker flag in RTP packet if true
	virtual message_ptr packetize(shared_ptr<binary> payload, bool mark);

private:
	static const auto RtpHeaderSize = 12;
	static const auto RtpExtHeaderCvoSize = 8;
};

// Generic audio RTP packetizer
template <uint32_t DEFAULT_CLOCK_RATE>
class RTC_CPP_EXPORT AudioRtpPacketizer final : public RtpPacketizer {
public:
	inline static const uint32_t DefaultClockRate = DEFAULT_CLOCK_RATE;
	inline static const uint32_t defaultClockRate [[deprecated("Use DefaultClockRate")]] =
	    DEFAULT_CLOCK_RATE; // for backward compatibility

	AudioRtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig)
	    : RtpPacketizer(std::move(rtpConfig)) {}
};

// Audio RTP packetizers
using OpusRtpPacketizer = AudioRtpPacketizer<48000>;
using AACRtpPacketizer = AudioRtpPacketizer<48000>;

// Dummy wrapper for backward compatibility, do not use
class RTC_CPP_EXPORT PacketizationHandler final : public MediaHandler {
public:
	PacketizationHandler(shared_ptr<RtpPacketizer> packetizer)
	    : mPacketizer(std::move(packetizer)) {}

	inline void outgoing(message_vector &messages, const message_callback &send) {
		return mPacketizer->outgoing(messages, send);
	}

private:
	shared_ptr<RtpPacketizer> mPacketizer;
};

// Audio packetization handlers for backward compatibility, do not use
using OpusPacketizationHandler [[deprecated("Add OpusRtpPacketizer directly")]] =
    PacketizationHandler;
using AACPacketizationHandler [[deprecated("Add AACRtpPacketizer directly")]] =
    PacketizationHandler;

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTP_PACKETIZER_H */
