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

size_t Channel::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

size_t Channel::bufferedAmount() const { return mBufferedAmount; }

size_t Channel::availableAmount() const { return 0; }

void Channel::onOpen(std::function<void()> callback) { mOpenCallback = callback; }

void Channel::onClosed(std::function<void()> callback) { mClosedCallback = callback; }

void Channel::onError(std::function<void(string error)> callback) { mErrorCallback = callback; }

void Channel::onMessage(std::function<void(message_variant data)> callback) {
	mMessageCallback = callback;

	// Pass pending messages
	while (auto message = receive())
		mMessageCallback(*message);
}

void Channel::onMessage(std::function<void(binary data)> binaryCallback,
                        std::function<void(string data)> stringCallback) {
	onMessage([binaryCallback, stringCallback](std::variant<binary, string> data) {
		std::visit(overloaded{binaryCallback, stringCallback}, std::move(data));
	});
}

void Channel::onBufferedAmountLow(std::function<void()> callback) {
	mBufferedAmountLowCallback = callback;
}

void Channel::setBufferedAmountLowThreshold(size_t amount) { mBufferedAmountLowThreshold = amount; }

void Channel::onAvailable(std::function<void()> callback) { mAvailableCallback = callback; }

void Channel::triggerOpen() { mOpenCallback(); }

void Channel::triggerClosed() { mClosedCallback(); }

void Channel::triggerError(string error) { mErrorCallback(error); }

void Channel::triggerAvailable(size_t count) {
	if (count == 1)
		mAvailableCallback();

	while (mMessageCallback && count--) {
		auto message = receive();
		if (!message)
			break;
		mMessageCallback(*message);
	}
}

void Channel::triggerBufferedAmount(size_t amount) {
	size_t previous = mBufferedAmount.exchange(amount);
	size_t threshold = mBufferedAmountLowThreshold.load();
	if (previous > threshold && amount <= threshold)
		mBufferedAmountLowCallback();
}

void Channel::resetCallbacks() {
	mOpenCallback = nullptr;
	mClosedCallback = nullptr;
	mErrorCallback = nullptr;
	mMessageCallback = nullptr;
	mAvailableCallback = nullptr;
	mBufferedAmountLowCallback = nullptr;
}

} // namespace rtc
