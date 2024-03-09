/**
 * Copyright (c) 2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_AV1_RTP_PACKETIZER_H
#define RTC_AV1_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "nalunit.hpp"
#include "rtppacketizer.hpp"

namespace rtc {

// RTP packetization of AV1 payload
class RTC_CPP_EXPORT AV1RtpPacketizer final : public RtpPacketizer {
public:
	// Default clock rate for AV1 in RTP
	inline static const uint32_t defaultClockRate = 90 * 1000;

	// Define how OBUs are seperated in a AV1 Sample
	enum class Packetization {
		Obu = RTC_OBU_PACKETIZED_OBU,
		TemporalUnit = RTC_OBU_PACKETIZED_TEMPORAL_UNIT,
	};

	// Constructs AV1 payload packetizer with given RTP configuration.
	// @note RTP configuration is used in packetization process which may change some configuration
	// properties such as sequence number.
	AV1RtpPacketizer(Packetization packetization, shared_ptr<RtpPacketizationConfig> rtpConfig,
	                 uint16_t maxFragmentSize = NalUnits::defaultMaximumFragmentSize);

	void outgoing(message_vector &messages, const message_callback &send) override;

private:
	shared_ptr<NalUnits> splitMessage(binary_ptr message);
	std::vector<shared_ptr<binary>> packetizeObu(binary_ptr message, uint16_t maxFragmentSize);

	const uint16_t maxFragmentSize;
	const Packetization packetization;
	std::shared_ptr<binary> sequenceHeader;
};

// For backward compatibility, do not use
using AV1PacketizationHandler [[deprecated("Add AV1RtpPacketizer directly")]] = PacketizationHandler;

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_AV1_RTP_PACKETIZER_H */
