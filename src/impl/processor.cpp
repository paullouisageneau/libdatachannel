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

namespace rtc::impl {

Processor::Processor(size_t limit) : mTasks(limit) {}

Processor::~Processor() { join(); }

void Processor::join() {
	std::unique_lock lock(mMutex);
	mCondition.wait(lock, [this]() { return !mPending && mTasks.empty(); });
}

void Processor::schedule() {
	std::unique_lock lock(mMutex);
	if (auto next = mTasks.tryPop()) {
		ThreadPool::Instance().enqueue(std::move(*next));
	} else {
		// No more tasks
		mPending = false;
		mCondition.notify_all();
	}
}

} // namespace rtc::impl
