/**
 * Copyright (c) 2024 Sean DuBois <sean@siobud.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include <memory>

#include "pacinghandler.hpp"

#include "impl/internals.hpp"
#include "impl/threadpool.hpp"

namespace rtc {

PacingHandler::PacingHandler(double bitsPerSecond, std::chrono::milliseconds sendInterval, size_t maxQueueAmount)
    : mBytesPerSecond(bitsPerSecond / 8), mBudget(0.), mSendInterval(sendInterval), mMaxQueueAmount(maxQueueAmount) {}

void PacingHandler::setBitrate(double bitsPerSecond) {
	std::lock_guard<std::mutex> lock(mParamsMutex);
    mBytesPerSecond = bitsPerSecond / 8;
}

void PacingHandler::setMaxQueueAmount(size_t maxQueueAmount) {
	std::lock_guard<std::mutex> lock(mParamsMutex);
    mMaxQueueAmount = maxQueueAmount;
}

void PacingHandler::onOverflow(std::function<void()> callback) {
	mOverflowCallback = std::move(callback);
}

void PacingHandler::schedule(const message_callback &send, std::chrono::milliseconds scheduleInterval) {
	if (!mHaveScheduled.exchange(true))
		impl::ThreadPool::Instance().schedule(scheduleInterval,
		                                      weak_bind(&PacingHandler::run, this, send));
}

void PacingHandler::run(const message_callback &send) {
	const std::lock_guard<std::mutex> lock(mMutex);
	mHaveScheduled.store(false);

	double bytesPerSecond;
	{
		std::lock_guard<std::mutex> lockParams(mParamsMutex);
		bytesPerSecond = mBytesPerSecond;
	}

	// Update the budget and cap it
	auto now = std::chrono::high_resolution_clock::now();
	auto newBudget = std::chrono::duration<double>(now - mLastRun).count() * bytesPerSecond;
	auto maxBudget = std::chrono::duration<double>(mSendInterval).count() * bytesPerSecond;
	mBudget = std::min(mBudget + newBudget, maxBudget);
	mLastRun = std::chrono::high_resolution_clock::now();

	// Send packets while there is budget, allow a single partial packet over budget
	while (!mRtpBuffer.empty() && mBudget > 0) {
		auto size = int(mRtpBuffer.front()->size());
		send(std::move(mRtpBuffer.front()));
		mRtpBuffer.pop();
		mBudget -= size;
	}

	auto scheduleInterval = std::chrono::duration_cast<std::chrono::milliseconds>(mSendInterval - (std::chrono::high_resolution_clock::now() - mLastRun));

	if (!mRtpBuffer.empty()) {
		schedule(send, std::max(scheduleInterval, std::chrono::milliseconds(0)));
	}
}

void PacingHandler::outgoing(message_vector &messages, const message_callback &send) {

	std::lock_guard<std::mutex> lock(mMutex);

	size_t maxQueueAmount;
    {
        std::lock_guard<std::mutex> lockParams(mParamsMutex);
        maxQueueAmount = mMaxQueueAmount;
    }

	for (auto& m : messages) {
		if (maxQueueAmount != 0 && mRtpBuffer.size() >= maxQueueAmount) {
			mOverflowCallback();
			break;
		}
		mRtpBuffer.push(std::move(m));
	}
	messages.clear();

	schedule(send, std::chrono::milliseconds(0));
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
