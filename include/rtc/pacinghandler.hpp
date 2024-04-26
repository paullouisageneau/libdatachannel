/**
 * Copyright (c) 2024 Sean DuBois <sean@siobud.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_PACING_HANDLER_H
#define RTC_PACING_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "utils.hpp"

#include <atomic>
#include <queue>

namespace rtc {

// Paced sending of RTP packets. It takes a stream of RTP packets that can have an uneven bitrate
// and delivers them in a smoother manner by sending a fixed size of them on an interval
class RTC_CPP_EXPORT PacingHandler : public MediaHandler {
public:
	PacingHandler(double bitsPerSecond, std::chrono::milliseconds sendInterval);

	void outgoing(message_vector &messages, const message_callback &send) override;

private:
	std::atomic<bool> mHaveScheduled = false;

	double mBytesPerSecond;
	double mBudget;

	std::chrono::milliseconds mSendInterval;
	std::chrono::time_point<std::chrono::high_resolution_clock> mLastRun;

	std::mutex mMutex;
	std::queue<message_ptr> mRtpBuffer;

	void schedule(const message_callback &send);
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_PACING_HANDLER_H
