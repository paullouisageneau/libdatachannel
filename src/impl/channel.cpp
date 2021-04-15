/**
 * Copyright (c) 2019-2021 Paul-Louis Ageneau
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

namespace rtc::impl {

void Channel::triggerOpen() {
	mOpenTriggered = true;
	openCallback();
	flushPendingMessages();
}

void Channel::triggerClosed() { closedCallback(); }

void Channel::triggerError(string error) { errorCallback(std::move(error)); }

void Channel::triggerAvailable(size_t count) {
	if (count == 1)
		availableCallback();

	flushPendingMessages();
}

void Channel::triggerBufferedAmount(size_t amount) {
	size_t previous = bufferedAmount.exchange(amount);
	size_t threshold = bufferedAmountLowThreshold.load();
	if (previous > threshold && amount <= threshold)
		bufferedAmountLowCallback();
}

void Channel::flushPendingMessages() {
	if (!mOpenTriggered)
		return;

	while (messageCallback) {
		auto next = receive();
		if (!next)
			break;

		messageCallback(*next);
	}
}

void Channel::resetOpenCallback() {
	mOpenTriggered = false;
	openCallback = nullptr;
}

void Channel::resetCallbacks() {
	mOpenTriggered = false;
	openCallback = nullptr;
	closedCallback = nullptr;
	errorCallback = nullptr;
	availableCallback = nullptr;
	bufferedAmountLowCallback = nullptr;
	messageCallback = nullptr;
}

} // namespace rtc::impl
