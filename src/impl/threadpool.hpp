/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_THREADPOOL_H
#define RTC_IMPL_THREADPOOL_H

#include "common.hpp"
#include "init.hpp"
#include "internals.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rtc::impl {

class ThreadPool final {
public:
	using clock = std::chrono::steady_clock;

	static ThreadPool &Instance();

	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;
	ThreadPool(ThreadPool &&) = delete;
	ThreadPool &operator=(ThreadPool &&) = delete;

	int count() const;
	void spawn(int count = 1);
	void join();
	void clear();
	void run();
	bool runOne();

	static void start(void *userContext) noexcept {
		auto *self = static_cast<ThreadPool *>(userContext);
		self->spawn();
	}

	static void schedule(std::chrono::steady_clock::time_point deadline, std::function<void()> task,
	                     void *userContext) noexcept {
		auto *self = static_cast<ThreadPool *>(userContext);
		std::unique_lock lock(self->mMutex);

		self->mTasks.push({deadline, [task = std::move(task)]() { task(); }});
		self->mTasksCondition.notify_one();
	}

private:
	ThreadPool();
	~ThreadPool();

	std::function<void()> dequeue(); // returns null function if joining

	std::vector<std::thread> mWorkers;
	std::atomic<int> mBusyWorkers = 0;
	std::atomic<bool> mJoining = false;

	struct Task {
		clock::time_point time;
		std::function<void()> func;
		bool operator>(const Task &other) const { return time > other.time; }
		bool operator<(const Task &other) const { return time < other.time; }
	};
	std::priority_queue<Task, std::deque<Task>, std::greater<Task>> mTasks;

	std::condition_variable mTasksCondition, mWaitingCondition;
	mutable std::mutex mMutex, mWorkersMutex;
};

} // namespace rtc::impl

#endif
