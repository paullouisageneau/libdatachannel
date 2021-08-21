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

#include "impl/channel.hpp"
#include "impl/internals.hpp"

namespace rtc {

Channel::~Channel() { impl()->resetCallbacks(); }

Channel::Channel(impl_ptr<impl::Channel> impl) : CheshireCat<impl::Channel>(std::move(impl)) {}

size_t Channel::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

size_t Channel::bufferedAmount() const { return impl()->bufferedAmount; }

void Channel::onOpen(std::function<void()> callback) { impl()->openCallback = callback; }

void Channel::onClosed(std::function<void()> callback) { impl()->closedCallback = callback; }

void Channel::onError(std::function<void(string error)> callback) {
	impl()->errorCallback = callback;
}

void Channel::onMessage(std::function<void(message_variant data)> callback) {
	impl()->messageCallback = callback;
	impl()->flushPendingMessages();
}

void Channel::onMessage(std::function<void(binary data)> binaryCallback,
                        std::function<void(string data)> stringCallback) {
	onMessage([binaryCallback, stringCallback](variant<binary, string> data) {
		std::visit(overloaded{binaryCallback, stringCallback}, std::move(data));
	});
}

void Channel::onBufferedAmountLow(std::function<void()> callback) {
	impl()->bufferedAmountLowCallback = callback;
}

void Channel::setBufferedAmountLowThreshold(size_t amount) {
	impl()->bufferedAmountLowThreshold = amount;
}

optional<message_variant> Channel::receive() { return impl()->receive(); }

optional<message_variant> Channel::peek() { return impl()->peek(); }

size_t Channel::availableAmount() const { return impl()->availableAmount(); }

void Channel::onAvailable(std::function<void()> callback) { impl()->availableCallback = callback; }

} // namespace rtc
