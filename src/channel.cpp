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

namespace {}

namespace rtc {

void Channel::onOpen(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(mCallbackMutex);
	mOpenCallback = callback;
}

void Channel::onClosed(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(mCallbackMutex);
	mClosedCallback = callback;
}

void Channel::onError(std::function<void(const string &error)> callback) {
	std::lock_guard<std::mutex> lock(mCallbackMutex);
	mErrorCallback = callback;
}

void Channel::onMessage(std::function<void(const std::variant<binary, string> &data)> callback) {
	std::lock_guard<std::mutex> lock(mCallbackMutex);
	mMessageCallback = callback;

	// Pass pending messages
	while (auto message = receive()) {
		// The callback might be changed from itself
		if (auto callback = getCallback(mMessageCallback))
			callback(*message);
	}
}

void Channel::onMessage(std::function<void(const binary &data)> binaryCallback,
                        std::function<void(const string &data)> stringCallback) {
	onMessage([binaryCallback, stringCallback](const std::variant<binary, string> &data) {
		std::visit(overloaded{binaryCallback, stringCallback}, data);
	});
}

void Channel::onAvailable(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(mCallbackMutex);
	mAvailableCallback = callback;
}

void Channel::onSent(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(mCallbackMutex);
	mSentCallback = callback;
}

void Channel::triggerOpen() {
	if (auto callback = getCallback(mOpenCallback))
		callback();
}

void Channel::triggerClosed() {
	if (auto callback = getCallback(mClosedCallback))
		callback();
}

void Channel::triggerError(const string &error) {
	if (auto callback = getCallback(mErrorCallback))
		callback(error);
}

void Channel::triggerAvailable(size_t available) {
	if (available == 1) {
		if (auto callback = getCallback(mAvailableCallback))
			callback();
	}
	while (available--) {
		auto message = receive();
		if (!message)
			break;
		// The callback might be changed from itself
		if (auto callback = getCallback(mMessageCallback))
			callback(*message);
	}
}

void Channel::triggerSent() {
	if (auto callback = getCallback(mSentCallback))
		callback();
}

} // namespace rtc

