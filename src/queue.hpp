/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#ifndef RTC_QUEUE_H
#define RTC_QUEUE_H

#include "include.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace rtc {

template <typename T> class Queue {
public:
	Queue();
	~Queue();

	void stop();
	bool empty() const;
	void push(const T &element);
	std::optional<T> pop();
	void wait();
	void wait(const std::chrono::milliseconds &duration);

private:
	std::queue<T> mQueue;
	std::condition_variable mCondition;
	std::atomic<bool> mStopping;

	mutable std::mutex mMutex;
};

template <typename T> Queue<T>::Queue() : mStopping(false) {}

template <typename T> Queue<T>::~Queue() { stop(); }

template <typename T> void Queue<T>::stop() {
	std::lock_guard<std::mutex> lock(mMutex);
	mStopping = true;
	mCondition.notify_all();
}

template <typename T> bool Queue<T>::empty() const {
	std::lock_guard<std::mutex> lock(mMutex);
	return mQueue.empty();
}

template <typename T> void Queue<T>::push(const T &element) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (mStopping)
		return;
	mQueue.push(element);
	mCondition.notify_one();
}

template <typename T> std::optional<T> Queue<T>::pop() {
	std::unique_lock<std::mutex> lock(mMutex);
	while (mQueue.empty()) {
		if (mStopping)
			return nullopt;
		mCondition.wait(lock);
	}

	std::optional<T> element = mQueue.front();
	mQueue.pop();
	return element;
}

template <typename T> void Queue<T>::wait() {
	std::unique_lock<std::mutex> lock(mMutex);
	if (mQueue.empty() && !mStopping)
		mCondition.wait(lock);
}

template <typename T> void Queue<T>::wait(const std::chrono::milliseconds &duration) {
	std::unique_lock<std::mutex> lock(mMutex);
	if (mQueue.empty() && !mStopping)
		mCondition.wait_for(lock, duration);
}

} // namespace rtc

#endif

