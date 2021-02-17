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

void joinThreadPoolInstance() { rtc::ThreadPool::Instance().join(); }

} // namespace

namespace rtc {

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
	std::scoped_lock lock(mMutex, mWorkersMutex);
	mJoining = false;
	while (count-- > 0)
		mWorkers.emplace_back(std::bind(&ThreadPool::run, this));
}

void ThreadPool::join() {
	{
		std::unique_lock lock(mMutex);
		mWaitingCondition.wait(lock, [&]() { return mWaitingWorkers == int(mWorkers.size()); });
		mJoining = true;
		mTasksCondition.notify_all();
	}

	std::unique_lock lock(mWorkersMutex);
	for (auto &w : mWorkers)
		w.join();

	mWorkers.clear();
}

void ThreadPool::run() {
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
		if (!mTasks.empty()) {
			if (mTasks.top().time <= clock::now()) {
				auto func = std::move(mTasks.top().func);
				mTasks.pop();
				return func;
			}

			++mWaitingWorkers;
			mWaitingCondition.notify_all();
			mTasksCondition.wait_until(lock, mTasks.top().time);

		} else {
			++mWaitingWorkers;
			mWaitingCondition.notify_all();
			mTasksCondition.wait(lock);
		}

		--mWaitingWorkers;
	}
	return nullptr;
}

} // namespace rtc
