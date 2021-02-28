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

#ifndef RTC_IMPL_PROCESSOR_H
#define RTC_IMPL_PROCESSOR_H

#include "common.hpp"
#include "init.hpp"
#include "queue.hpp"
#include "threadpool.hpp"

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>

namespace rtc::impl {

// Processed tasks in order by delegating them to the thread pool
class Processor final {
public:
	Processor(size_t limit = 0);
	~Processor();

	Processor(const Processor &) = delete;
	Processor &operator=(const Processor &) = delete;
	Processor(Processor &&) = delete;
	Processor &operator=(Processor &&) = delete;

	void join();

	template <class F, class... Args> void enqueue(F &&f, Args &&...args);

protected:
	void schedule();

	// Keep an init token
	const init_token mInitToken = Init::Token();

	Queue<std::function<void()>> mTasks;
	bool mPending = false; // true iff a task is pending in the thread pool

	mutable std::mutex mMutex;
	std::condition_variable mCondition;
};

template <class F, class... Args> void Processor::enqueue(F &&f, Args &&...args) {
	std::unique_lock lock(mMutex);
	auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
	auto task = [this, bound = std::move(bound)]() mutable {
		scope_guard guard(std::bind(&Processor::schedule, this)); // chain the next task
		return bound();
	};

	if (!mPending) {
		ThreadPool::Instance().enqueue(std::move(task));
		mPending = true;
	} else {
		mTasks.push(std::move(task));
	}
}

} // namespace rtc::impl

#endif
