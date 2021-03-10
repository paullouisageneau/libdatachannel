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

#include "threadpool.hpp"

#include <cstdlib>

namespace {

void joinThreadPoolInstance() { rtc::impl::ThreadPool::Instance().join(); }

} // namespace

namespace rtc::impl {

ThreadPool &ThreadPool::Instance() {
	static ThreadPool *instance = new ThreadPool;
	return *instance;
}

ThreadPool::ThreadPool() { std::atexit(joinThreadPoolInstance); }

ThreadPool::~ThreadPool() {}

int ThreadPool::count() const {
	std::unique_lock lock(mWorkersMutex);
	return int(mWorkers.size());
}

void ThreadPool::spawn(int count) {
	std::unique_lock lock(mWorkersMutex);
	while (count-- > 0)
		mWorkers.emplace_back(std::bind(&ThreadPool::run, this));
}

void ThreadPool::join() {
	{
		std::unique_lock lock(mMutex);
		mWaitingCondition.wait(lock, [&]() { return mBusyWorkers == 0; });
		mJoining = true;
		mTasksCondition.notify_all();
	}

	std::unique_lock lock(mWorkersMutex);
	for (auto &w : mWorkers)
		w.join();

	mWorkers.clear();

	mJoining = false;
}

void ThreadPool::run() {
	++mBusyWorkers;
	scope_guard guard([&]() { --mBusyWorkers; });
	while (runOne()) {
	}
}

bool ThreadPool::runOne() {
	if (auto task = dequeue()) {
		task();
		return true;
	}
	return false;
}

std::function<void()> ThreadPool::dequeue() {
	std::unique_lock lock(mMutex);
	while (!mJoining) {
		std::optional<clock::time_point> time;
		if (!mTasks.empty()) {
			time = mTasks.top().time;
			if (*time <= clock::now()) {
				auto func = std::move(mTasks.top().func);
				mTasks.pop();
				return func;
			}
		}

		--mBusyWorkers;
		scope_guard guard([&]() { ++mBusyWorkers; });
		mWaitingCondition.notify_all();
		if(time)
			mTasksCondition.wait_until(lock, *time);
		else
			mTasksCondition.wait(lock);
	}
	return nullptr;
}

} // namespace rtc::impl
