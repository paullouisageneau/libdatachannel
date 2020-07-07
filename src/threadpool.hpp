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

#ifndef RTC_THREADPOOL_H
#define RTC_THREADPOOL_H

#include "include.hpp"

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rtc {

template <class F, class... Args>
using invoke_future_t = std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

class ThreadPool final {
public:
	explicit ThreadPool(int count);
	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;
	ThreadPool(ThreadPool &&) = delete;
	ThreadPool &operator=(ThreadPool &&) = delete;
	~ThreadPool();

	int count() const;
	void spawn(int count = 1);
	void join();
	void run();
	bool runOne();

	template <class F, class... Args>
	auto enqueue(F &&f, Args &&... args) -> invoke_future_t<F, Args...>;

protected:
	std::function<void()> dequeue(); // returns null function if joining

	std::vector<std::thread> mWorkers;
	std::queue<std::function<void()>> mTasks;

	std::mutex mMutex;
	std::condition_variable mCondition;
	bool mJoining = false;
};

template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&... args) -> invoke_future_t<F, Args...> {
	std::unique_lock lock(mMutex);
	using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
	auto task = std::packaged_task<R()>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
	std::future<R> result = task.get_future();
	mTasks.emplace([task = std::move(task)]() { task(); });
	mCondition.notify_one();
	return result;
}

} // namespace rtc

#endif
