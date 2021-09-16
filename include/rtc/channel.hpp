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

#ifndef RTC_CHANNEL_H
#define RTC_CHANNEL_H

#include "common.hpp"
#include "message.hpp"

#include <atomic>
#include <functional>

namespace rtc {

namespace impl {
struct Channel;
}

class RTC_CPP_EXPORT Channel : private CheshireCat<impl::Channel> {
public:
	virtual ~Channel();

	virtual void close() = 0;
	virtual bool send(message_variant data) = 0; // returns false if buffered
	virtual bool send(const byte *data, size_t size) = 0;

	virtual bool isOpen() const = 0;
	virtual bool isClosed() const = 0;
	virtual size_t maxMessageSize() const; // max message size in a call to send
	virtual size_t bufferedAmount() const; // total size buffered to send

	void onOpen(std::function<void()> callback);
	void onClosed(std::function<void()> callback);
	void onError(std::function<void(string error)> callback);

	void onMessage(std::function<void(message_variant data)> callback);
	void onMessage(std::function<void(binary data)> binaryCallback,
	               std::function<void(string data)> stringCallback);

	void onBufferedAmountLow(std::function<void()> callback);
	void setBufferedAmountLowThreshold(size_t amount);

	// Extended API
	optional<message_variant> receive(); // only if onMessage unset
	optional<message_variant> peek();    // only if onMessage unset
	size_t availableAmount() const;      // total size available to receive
	void onAvailable(std::function<void()> callback);

protected:
	Channel(impl_ptr<impl::Channel> impl);
};

} // namespace rtc

#endif // RTC_CHANNEL_H
