/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "processor.hpp"

namespace rtc {

Processor::Processor(size_t limit) : mTasks(limit) {}

Processor::~Processor() { join(); }

void Processor::join() {
	// We need to detect situations where the thread pool does not execute a pending task at exit
	std::optional<unsigned int> counter;
	while (true) {
		std::shared_future<void> pending;
		{
			std::unique_lock lock(mMutex);
			if (!mPending                               // no pending task
			    || (counter && *counter == mCounter)) { // or no scheduled task after the last one

				// Processing is stopped, clear everything and return
				mPending.reset();
				while (!mTasks.empty())
					mTasks.pop();

				return;
			}

			pending = *mPending;
			counter = mCounter;
		}

		// Wait for the pending task
		pending.wait();
	}
}

void Processor::schedule() {
	std::unique_lock lock(mMutex);
	if (auto next = mTasks.tryPop()) {
		mPending = ThreadPool::Instance().enqueue(std::move(*next)).share();
		++mCounter;
	} else {
		mPending.reset(); // No more tasks
	}
}

} // namespace rtc
