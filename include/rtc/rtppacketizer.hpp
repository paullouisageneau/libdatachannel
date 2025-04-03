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
	/// Default maximum fragment size (for video packetizers)
	inline static const size_t DefaultMaxFragmentSize = RTC_DEFAULT_MAX_FRAGMENT_SIZE;

	/// Clock rate for video in RTP
	inline static const uint32_t VideoClockRate = 90 * 1000;

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
	/// Fragment data into payloads
	/// Default implementation returns data as a single payload
	/// @param message Input data
	virtual std::vector<binary> fragment(binary data);

	/// Creates an RTP packet for a payload
	/// @note This function increases the sequence number.
	/// @param payload RTP payload
	/// @param mark Set marker flag in RTP packet if true
	virtual message_ptr packetize(const binary &payload, bool mark);

	// For backward compatibility, do not use
	[[deprecated]] virtual message_ptr packetize(shared_ptr<binary> payload, bool mark);

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
using PCMARtpPacketizer = AudioRtpPacketizer<8000>;
using PCMURtpPacketizer = AudioRtpPacketizer<8000>;

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
