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

#ifndef RTC_INCLUDE_H
#define RTC_INCLUDE_H

#ifndef RTC_ENABLE_MEDIA
#define RTC_ENABLE_MEDIA 1
#endif

#ifndef RTC_ENABLE_WEBSOCKET
#define RTC_ENABLE_WEBSOCKET 1
#endif

#ifdef _WIN32
#define RTC_CPP_EXPORT __declspec(dllexport)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 // Windows 8
#endif
#ifdef _MSC_VER
#pragma warning(disable:4251) // disable "X needs to have dll-interface..."
#endif
#else
#define RTC_CPP_EXPORT
#endif

#include "log.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rtc {

using std::byte;
using std::string;
using std::string_view;
using binary = std::vector<byte>;

using std::nullopt;

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

const size_t MAX_NUMERICNODE_LEN = 48; // Max IPv6 string representation length
const size_t MAX_NUMERICSERV_LEN = 6;  // Max port string representation length

const uint16_t DEFAULT_SCTP_PORT = 5000;          // SCTP port to use by default
const size_t DEFAULT_MAX_MESSAGE_SIZE = 65536;    // Remote max message size if not specified in SDP
const size_t LOCAL_MAX_MESSAGE_SIZE = 256 * 1024; // Local max message size

const size_t RECV_QUEUE_LIMIT = 1024 * 1024; // Max per-channel queue size

const int THREADPOOL_SIZE = 4; // Number of threads in the global thread pool

// overloaded helper
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// weak_ptr bind helper
template <typename F, typename T, typename... Args> auto weak_bind(F &&f, T *t, Args &&... _args) {
	return [bound = std::bind(f, t, _args...), weak_this = t->weak_from_this()](auto &&... args) {
		using result_type = typename decltype(bound)::result_type;
		if (auto shared_this = weak_this.lock())
			return bound(args...);
		else
			return static_cast<result_type>(false);
	};
}

// scope_guard helper
class scope_guard {
public:
	scope_guard(std::function<void()> func) : function(std::move(func)) {}
	scope_guard(scope_guard &&other) = delete;
	scope_guard(const scope_guard &) = delete;
	void operator=(const scope_guard &) = delete;

	~scope_guard() {
		if (function)
			function();
	}

private:
	std::function<void()> function;
};

template <typename... Args> class synchronized_callback {
public:
	synchronized_callback() = default;
	synchronized_callback(synchronized_callback &&cb) { *this = std::move(cb); }
	synchronized_callback(const synchronized_callback &cb) { *this = cb; }
	synchronized_callback(std::function<void(Args...)> func) { *this = std::move(func); }
	~synchronized_callback() { *this = nullptr; }

	synchronized_callback &operator=(synchronized_callback &&cb) {
		std::scoped_lock lock(mutex, cb.mutex);
		callback = std::move(cb.callback);
		cb.callback = nullptr;
		return *this;
	}

	synchronized_callback &operator=(const synchronized_callback &cb) {
		std::scoped_lock lock(mutex, cb.mutex);
		callback = cb.callback;
		return *this;
	}

	synchronized_callback &operator=(std::function<void(Args...)> func) {
		std::lock_guard lock(mutex);
		callback = std::move(func);
		return *this;
	}

	void operator()(Args... args) const {
		std::lock_guard lock(mutex);
		if (callback)
			callback(std::move(args)...);
	}

	operator bool() const {
		std::lock_guard lock(mutex);
		return callback ? true : false;
	}

	std::function<void(Args...)> wrap() const {
		return [this](Args... args) { (*this)(std::move(args)...); };
	}

private:
	std::function<void(Args...)> callback;
	mutable std::recursive_mutex mutex;
};
} // namespace rtc

#endif
