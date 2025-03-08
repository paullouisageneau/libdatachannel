#ifndef RTC_VP8_RTP_DEPACKETIZER_H
#define RTC_VP8_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "mediahandler.hpp"
#include "message.hpp"
#include "rtp.hpp"
#include "vp8nalunit.hpp"

#include <vector>
#include <cstdint>
#include <algorithm>
#include <memory>

namespace rtc {

/*
 * Minimal VP8 depacketizer example, paralleling H265RtpDepacketizer.
 * It collects consecutive RTP packets sharing the same timestamp,
 * then calls buildFrame() to combine them (sorted by sequence number)
 * into a single “frame” or partial frame. If the last packet's M-bit
 * is zero, you may be missing more data, but for simplicity we decode
 * partial frames anyway.
 */
class RTC_CPP_EXPORT VP8RtpDepacketizer : public MediaHandler {
public:
	static constexpr uint32_t ClockRate = 90000; // 90 kHz for video

	VP8RtpDepacketizer() = default;
	~VP8RtpDepacketizer() override = default;

	void incoming(message_vector &messages, const message_callback &send) override;

private:
	std::vector<message_ptr> mRtpBuffer;

	// Combine all packets [first .. last) with the same timestamp into one frame
	message_vector buildFrame(std::vector<message_ptr>::iterator first,
	                          std::vector<message_ptr>::iterator last,
	                          uint8_t payloadType,
	                          uint32_t timestamp);

	static bool seqLess(uint16_t a, uint16_t b) {
		// Sort by ascending sequence number, with 16-bit wrap
		return (int16_t)(a - b) < 0;
	}
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_VP8_RTP_DEPACKETIZER_H