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

#include <atomic>
#include <map>
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

	// When true, incoming() reassembles SFrame RFC packets first. Written by media() on the
	// negotiating thread while incoming() runs on the media thread.
	std::atomic<bool> enableSFrame = false;

	// enableSFrame as incoming() read it for the batch in progress. Media thread only.
	// Reading the flag again part way through would let a renegotiation split one batch
	// between the two paths and deliver reassembled ciphertext as media.
	bool sframeBatch = false;

	// Protected so SFrame depacketizers can assemble, then decrypt.
	void incoming(message_vector &messages, const message_callback &send) override;

private:
	bool isStaleSFrameTimestamp(uint32_t timestamp) const;
	void bufferSFramePacket(message_ptr packet, uint32_t timestamp);
	void releaseSFrameGroup(size_t index, message_vector &result);
	void flushSFramesThrough(uint32_t timestamp, message_vector &result);
	void flushSFramesExceptNewest(message_vector &result);
	void emitCompleteSFrames(message_vector &result);
	void evictOverflowingSFrames();
	bool sframeGroupResolves(const message_buffer &packets);
	bool assembleSFrameObjects(const message_buffer &packets, message_vector &result,
	                           bool dryRun = false);
	std::optional<size_t> validateSFrameRun(const message_vector &run);
	bool assembleSFrameObject(const message_vector &run, message_vector &result);

	message_buffer mBuffer;

	// One RTP timestamp's packets. Held in the order the groups were first seen, not in
	// timestamp order: the timestamp is the peer's to choose, and ordering structural
	// decisions by it lets one packet with a far-future timestamp sit at the head forever.
	struct SFrameGroup {
		uint32_t timestamp;
		message_buffer packets;
	};
	std::vector<SFrameGroup> mSFrames;
	size_t mSFrameBufferedBytes = 0;

	// Newest timestamp already delivered, so a straggler for it can be dropped.
	uint32_t mSFrameLastEmitted = 0;
	bool mSFrameEmitted = false;

	// The peer decides how much is held here, so cap it in both directions.
	static constexpr size_t MaxSFrames = 8;
	static constexpr size_t MaxSFrameBufferedBytes = 4 * 1024 * 1024;
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
