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

#include <functional>
#include <mutex>
#include <variant>

namespace rtc {

class Channel {
public:
	virtual void close(void) = 0;
	virtual void send(const std::variant<binary, string> &data) = 0;
	virtual std::optional<std::variant<binary, string>> receive() = 0;
	virtual bool isOpen(void) const = 0;
	virtual bool isClosed(void) const = 0;

	void onOpen(std::function<void()> callback);
	void onClosed(std::function<void()> callback);
	void onError(std::function<void(const string &error)> callback);

	void onMessage(std::function<void(const std::variant<binary, string> &data)> callback);
	void onMessage(std::function<void(const binary &data)> binaryCallback,
	               std::function<void(const string &data)> stringCallback);

	void onAvailable(std::function<void()> callback);

protected:
	virtual void triggerOpen(void);
	virtual void triggerClosed(void);
	virtual void triggerError(const string &error);
	virtual void triggerAvailable(size_t available);

private:
	std::function<void()> mOpenCallback;
	std::function<void()> mClosedCallback;
	std::function<void(const string &)> mErrorCallback;
	std::function<void(const std::variant<binary, string> &)> mMessageCallback;
	std::function<void()> mAvailableCallback;
	std::recursive_mutex mCallbackMutex;
};

} // namespace rtc

#endif // RTC_CHANNEL_H

