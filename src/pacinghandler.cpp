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

PacingHandler::PacingHandler(double bitsPerSecond, std::chrono::milliseconds sendInterval)
    : mBytesPerSecond(bitsPerSecond / 8), mBudget(0.), mSendInterval(sendInterval){};

void PacingHandler::schedule(const message_callback &send) {
	if (!mHaveScheduled.exchange(true)) {
		return;
	}

	impl::ThreadPool::Instance().schedule(mSendInterval, [this, weak_this = weak_from_this(),
	                                                      send]() {
		if (auto locked = weak_this.lock()) {
			const std::lock_guard<std::mutex> lock(mMutex);
			mHaveScheduled.store(false);

			// Update the budget and cap it
			auto newBudget =
			    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - mLastRun)
			        .count() *
			    mBytesPerSecond;
			auto maxBudget = std::chrono::duration<double>(mSendInterval).count() * mBytesPerSecond;
			mBudget = std::min(mBudget + newBudget, maxBudget);
			mLastRun = std::chrono::high_resolution_clock::now();

			// Send packets while there is budget, allow a single partial packet over budget
			while (!mRtpBuffer.empty() && mBudget > 0) {
				auto size = int(mRtpBuffer.front()->size());
				send(std::move(mRtpBuffer.front()));
				mRtpBuffer.pop();
				mBudget -= size;
			}

			if (!mRtpBuffer.empty()) {
				schedule(send);
			}
		}
	});
}

void PacingHandler::outgoing(message_vector &messages, const message_callback &send) {

	std::lock_guard<std::mutex> lock(mMutex);

	for (auto &m : messages) {
		mRtpBuffer.push(std::move(m));
	}
	messages.clear();

	schedule(send);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
