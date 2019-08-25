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

void Channel::onOpen(std::function<void()> callback) { mOpenCallback = callback; }

void Channel::onClosed(std::function<void()> callback) { mClosedCallback = callback; }

void Channel::onError(std::function<void(const string &error)> callback) {
	mErrorCallback = callback;
}

void Channel::onMessage(std::function<void(const std::variant<binary, string> &data)> callback) {
	mMessageCallback = callback;
}

void Channel::triggerOpen(void) {
	if (mOpenCallback)
		mOpenCallback();
}

void Channel::triggerClosed(void) {
	if (mClosedCallback)
		mClosedCallback();
}

void Channel::triggerError(const string &error) {
	if (mErrorCallback)
		mErrorCallback(error);
}

void Channel::triggerMessage(const std::variant<binary, string> &data) {
	if (mMessageCallback)
		mMessageCallback(data);
}

} // namespace rtc

