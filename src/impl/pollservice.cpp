/**
 * Copyright (c) 2022 Paul-Louis Ageneau
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

#include "pollservice.hpp"
#include "internals.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <cassert>

namespace rtc::impl {

using namespace std::chrono_literals;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

PollService &PollService::Instance() {
	static PollService *instance = new PollService;
	return *instance;
}

PollService::PollService() : mStopped(true) {}

PollService::~PollService() {}

void PollService::start() {
	mSocks = std::make_unique<SocketMap>();
	mStopped = false;
	mThread = std::thread(&PollService::runLoop, this);
}

void PollService::join() {
	std::unique_lock lock(mMutex);
	if (std::exchange(mStopped, true))
		return;

	lock.unlock();

	mInterrupter.interrupt();
	mThread.join();
	mSocks.reset();
}

void PollService::add(socket_t sock, Params params) {
	std::unique_lock lock(mMutex);
	assert(mSocks);

	mSocks->erase(sock);

	if (!params.callback)
		return;

	PLOG_VERBOSE << "Registering socket in poll service, direction=" << params.direction;
	auto until = params.timeout ? std::make_optional(clock::now() + *params.timeout) : nullopt;
	mSocks->emplace(sock, SocketEntry{std::move(params), std::move(until)});

	mInterrupter.interrupt();
}

void PollService::remove(socket_t sock) {
	std::unique_lock lock(mMutex);
	assert(mSocks);

	PLOG_VERBOSE << "Unregistering socket in poll service";
	mSocks->erase(sock);

	mInterrupter.interrupt();
}

void PollService::prepare(std::vector<struct pollfd> &pfds, optional<clock::time_point> &next) {
	std::unique_lock lock(mMutex);
	pfds.resize(1 + mSocks->size());
	next.reset();

	auto it = pfds.begin();
	mInterrupter.prepare(*it++);
	for (const auto &[sock, entry] : *mSocks) {
		it->fd = sock;
		switch (entry.params.direction) {
		case Direction::In:
			it->events = POLLIN;
			break;
		case Direction::Out:
			it->events = POLLOUT;
			break;
		default:
			it->events = POLLIN | POLLOUT;
			break;
		}
		if (entry.until)
			next = next ? std::min(*next, *entry.until) : *entry.until;

		++it;
	}
}

void PollService::process(std::vector<struct pollfd> &pfds) {
	std::unique_lock lock(mMutex);
	for (auto it = pfds.begin(); it != pfds.end(); ++it) {
		socket_t sock = it->fd;
		auto jt = mSocks->find(sock);
		if (jt == mSocks->end())
			continue; // removed

		try {
			auto &entry = jt->second;
			const auto &params = entry.params;

			if (it->revents & POLLNVAL || it->revents & POLLERR) {
				PLOG_VERBOSE << "Poll error event";
				auto callback = std::move(params.callback);
				mSocks->erase(sock);
				callback(Event::Error);
				continue;
			}

			if (it->revents & POLLIN || it->revents & POLLOUT) {
				entry.until =
				    params.timeout ? std::make_optional(clock::now() + *params.timeout) : nullopt;

				auto callback = params.callback;

				if (it->revents & POLLIN) {
					PLOG_VERBOSE << "Poll in event";
					params.callback(Event::In);
				}

				if (it->revents & POLLOUT) {
					PLOG_VERBOSE << "Poll out event";
					params.callback(Event::Out);
				}

				continue;
			}

			if (entry.until && clock::now() >= *entry.until) {
				PLOG_VERBOSE << "Poll timeout event";
				auto callback = std::move(params.callback);
				mSocks->erase(sock);
				callback(Event::Timeout);
				continue;
			}

		} catch (const std::exception &e) {
			PLOG_WARNING << e.what();
			mSocks->erase(sock);
		}
	}
}

void PollService::runLoop() {
	try {
		PLOG_DEBUG << "Poll service started";
		assert(mSocks);

		std::vector<struct pollfd> pfds;
		optional<clock::time_point> next;
		while (!mStopped) {
			prepare(pfds, next);

			PLOG_VERBOSE << "Entering poll";
			int ret;
			do {
				int timeout = next ? duration_cast<milliseconds>(
				                         std::max(clock::duration::zero(), *next - clock::now()))
				                         .count()
				                   : -1;
				ret = ::poll(pfds.data(), pfds.size(), timeout);

			} while (ret < 0 && (sockerrno == SEINTR || sockerrno == SEAGAIN));

			PLOG_VERBOSE << "Exiting poll";

			if (ret < 0)
				throw std::runtime_error("Failed to wait for socket connection");

			process(pfds);
		}
	} catch (const std::exception &e) {
		PLOG_FATAL << "Poll service failed: " << e.what();
	}

	PLOG_DEBUG << "Poll service stopped";
}

std::ostream &operator<<(std::ostream &out, PollService::Direction direction) {
	const char *str;
	switch (direction) {
	case PollService::Direction::In:
		str = "in";
		break;
	case PollService::Direction::Out:
		str = "out";
		break;
	case PollService::Direction::Both:
		str = "both";
		break;
	default:
		str = "unknown";
		break;
	}
	return out << str;
}

} // namespace rtc::impl

#endif
