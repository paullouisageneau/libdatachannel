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

#ifndef RTC_TRANSPORT_H
#define RTC_TRANSPORT_H

#include "include.hpp"
#include "message.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace rtc {

using namespace std::placeholders;

class Transport {
public:
	enum class State { Disconnected, Connecting, Connected, Completed, Failed };
	using state_callback = std::function<void(State state)>;

	Transport(std::shared_ptr<Transport> lower = nullptr, state_callback callback = nullptr)
	    : mLower(std::move(lower)), mStateChangeCallback(std::move(callback)) {
	}

	virtual ~Transport() {
		stop();
		if (mLower)
			mLower->onRecv(nullptr); // doing it on stop could cause a deadlock
	}

	virtual bool stop() {
		return !mShutdown.exchange(true);
	}

	void registerIncoming() {
		if (mLower)
			mLower->onRecv(std::bind(&Transport::incoming, this, _1));
	}

	void onRecv(message_callback callback) { mRecvCallback = std::move(callback); }
	State state() const { return mState; }

	virtual bool send(message_ptr message) { return outgoing(message); }

protected:
	void recv(message_ptr message) { mRecvCallback(message); }
	void changeState(State state) {
		if (mState.exchange(state) != state)
			mStateChangeCallback(state);
	}

	virtual void incoming(message_ptr message) { recv(message); }
	virtual bool outgoing(message_ptr message) {
		if (mLower)
			return mLower->send(message);
		else
			return false;
	}

private:
	std::shared_ptr<Transport> mLower;
	synchronized_callback<State> mStateChangeCallback;
	synchronized_callback<message_ptr> mRecvCallback;

	std::atomic<State> mState = State::Disconnected;
	std::atomic<bool> mShutdown = false;
};

} // namespace rtc

#endif
