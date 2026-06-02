#ifndef RTC_IMPL_ASIO_H
#define RTC_IMPL_ASIO_H

#include "common.hpp"
#include "init.hpp"
#include "internals.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rtc::impl {

template <class F, class... Args>
using invoke_future_t = std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

class Asio {
public:
	using clock = std::chrono::steady_clock;

	static Asio &Instance();

	template <class F, class... Args>
	auto enqueue(F &&f, Args &&...args) noexcept -> invoke_future_t<F, Args...>;

	template <class F, class... Args>
	auto schedule(clock::duration delay, F &&f, Args &&...args) noexcept
	    -> invoke_future_t<F, Args...>;

	template <class F, class... Args>
	auto schedule(clock::time_point time, F &&f, Args &&...args) noexcept
	    -> invoke_future_t<F, Args...>;

	void init(AsioSettings const &s);
	void start();
	void stop();

private:
	AsioSettings mSettings;

	Asio() {}
};

template <class F, class... Args>
auto Asio::enqueue(F &&f, Args &&...args) noexcept -> invoke_future_t<F, Args...> {
	return schedule(clock::now(), std::forward<F>(f), std::forward<Args>(args)...);
}

template <class F, class... Args>
auto Asio::schedule(clock::duration delay, F &&f, Args &&...args) noexcept
    -> invoke_future_t<F, Args...> {
	return schedule(clock::now() + delay, std::forward<F>(f), std::forward<Args>(args)...);
}

template <class F, class... Args>
auto Asio::schedule(clock::time_point time, F &&f, Args &&...args) noexcept
    -> invoke_future_t<F, Args...> {
	using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
	auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
	auto task = std::make_shared<std::packaged_task<R()>>([bound = std::move(bound)]() mutable {
		try {
			return bound();
		} catch (const std::exception &e) {
			PLOG_WARNING << e.what();
			throw;
		}
	});

	std::future<R> result = task->get_future();

	mSettings.scheduleTask(time, [task = std::move(task)]() { (*task)(); }, mSettings.userContext);

	return result;
}

} // namespace rtc::impl

#endif
