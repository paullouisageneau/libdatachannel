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

#ifndef RTC_CHANNEL_H
#define RTC_CHANNEL_H

#include "include.hpp"
#include "message.hpp"

#include <atomic>
#include <functional>
#include <variant>

namespace rtc {

class RTC_CPP_EXPORT Channel {
public:
	Channel() = default;
	virtual ~Channel() = default;

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
	virtual std::optional<message_variant> receive() = 0; // only if onMessage unset
	virtual std::optional<message_variant> peek() = 0;    // only if onMessage unset
	virtual size_t availableAmount() const;               // total size available to receive
	void onAvailable(std::function<void()> callback);

protected:
	virtual void triggerOpen();
	virtual void triggerClosed();
	virtual void triggerError(string error);
	virtual void triggerAvailable(size_t count);
	virtual void triggerBufferedAmount(size_t amount);

	void resetCallbacks();

private:
	synchronized_callback<> mOpenCallback;
	synchronized_callback<> mClosedCallback;
	synchronized_callback<string> mErrorCallback;
	synchronized_callback<message_variant> mMessageCallback;
	synchronized_callback<> mAvailableCallback;
	synchronized_callback<> mBufferedAmountLowCallback;

	std::atomic<size_t> mBufferedAmount = 0;
	std::atomic<size_t> mBufferedAmountLowThreshold = 0;
};

} // namespace rtc

#endif // RTC_CHANNEL_H
