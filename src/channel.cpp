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

#include "channel.hpp"

namespace rtc {

void Channel::onOpen(std::function<void()> callback) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	mOpenCallback = callback;
}

void Channel::onClosed(std::function<void()> callback) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	mClosedCallback = callback;
}

void Channel::onError(std::function<void(const string &error)> callback) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	mErrorCallback = callback;
}

void Channel::onMessage(std::function<void(const std::variant<binary, string> &data)> callback) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	mMessageCallback = callback;

	// Pass pending messages
	while (auto message = receive()) {
		mMessageCallback(*message);
	}
}

void Channel::onMessage(std::function<void(const binary &data)> binaryCallback,
                        std::function<void(const string &data)> stringCallback) {
	onMessage([binaryCallback, stringCallback](const std::variant<binary, string> &data) {
		std::visit(overloaded{binaryCallback, stringCallback}, data);
	});
}

void Channel::onAvailable(std::function<void()> callback) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	mAvailableCallback = callback;
}

void Channel::triggerOpen(void) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	if (mOpenCallback)
		mOpenCallback();
}

void Channel::triggerClosed(void) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	if (mClosedCallback)
		mClosedCallback();
}

void Channel::triggerError(const string &error) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	if (mErrorCallback)
		mErrorCallback(error);
}

void Channel::triggerAvailable(size_t available) {
	std::lock_guard<std::recursive_mutex> lock(mCallbackMutex);
	if (mAvailableCallback && available == 1) {
		mAvailableCallback();
	}
	// The callback might be changed from itself
	while (mMessageCallback && available--) {
		auto message = receive();
		if (!message)
			break;
		mMessageCallback(*message);
	}
}

} // namespace rtc

