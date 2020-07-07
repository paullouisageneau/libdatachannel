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

#ifndef RTC_PROCESSOR_H
#define RTC_PROCESSOR_H

#include "include.hpp"
#include "threadpool.hpp"

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>

namespace rtc {

// Processed tasks in order by delegating them to the thread pool
class Processor final {
public:
	Processor() = default;
	~Processor();

	Processor(const Processor &) = delete;
	Processor &operator=(const Processor &) = delete;
	Processor(Processor &&) = delete;
	Processor &operator=(Processor &&) = delete;

	void join();

	template <class F, class... Args>
	auto enqueue(F &&f, Args &&... args) -> invoke_future_t<F, Args...>;

protected:
	void schedule();

	std::queue<std::function<void()>> mTasks;
	bool mPending = false; // true iff a task is pending in the thread pool

	mutable std::mutex mMutex;
	std::condition_variable mCondition;
};

template <class F, class... Args>
auto Processor::enqueue(F &&f, Args &&... args) -> invoke_future_t<F, Args...> {
	std::unique_lock lock(mMutex);
	using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
	auto task = std::make_shared<std::packaged_task<R()>>(
	    std::bind(std::forward<F>(f), std::forward<Args>(args)...));
	std::future<R> result = task->get_future();

	auto bundle = [this, task = std::move(task)]() {
		try {
			(*task)();
		} catch (const std::exception &e) {
			PLOG_WARNING << "Unhandled exception in task: " << e.what();
		}
		schedule(); // chain the next task
	};

	if (!mPending) {
		ThreadPool::Instance().enqueue(std::move(bundle));
		mPending = true;
	} else {
		mTasks.emplace(std::move(bundle));
	}

	return result;
}

} // namespace rtc

#endif
