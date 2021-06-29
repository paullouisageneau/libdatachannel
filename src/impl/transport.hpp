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

#ifndef RTC_IMPL_TRANSPORT_H
#define RTC_IMPL_TRANSPORT_H

#include "common.hpp"
#include "internals.hpp"
#include "message.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace rtc::impl {

class Transport {
public:
	enum class State { Disconnected, Connecting, Connected, Completed, Failed };
	using state_callback = std::function<void(State state)>;

	Transport(shared_ptr<Transport> lower = nullptr, state_callback callback = nullptr)
	    : mLower(std::move(lower)), mStateChangeCallback(std::move(callback)) {}

	virtual ~Transport() { stop(); }

	virtual void start() { mStopped = false; }

	virtual bool stop() {
		if (mStopped.exchange(true))
			return false;

		// We don't want incoming() to be called by the lower layer anymore
		if (mLower) {
			PLOG_VERBOSE << "Unregistering incoming callback";
			mLower->onRecv(nullptr);
		}
		return true;
	}

	void registerIncoming() {
		if (mLower) {
			PLOG_VERBOSE << "Registering incoming callback";
			mLower->onRecv(std::bind(&Transport::incoming, this, std::placeholders::_1));
		}
	}

	void onRecv(message_callback callback) { mRecvCallback = std::move(callback); }
	void onStateChange(state_callback callback) { mStateChangeCallback = std::move(callback); }
	State state() const { return mState; }

	virtual bool send(message_ptr message) { return outgoing(message); }

protected:
	void recv(message_ptr message) {
		try {
			mRecvCallback(message);
		} catch (const std::exception &e) {
			PLOG_WARNING << e.what();
		}
	}
	void changeState(State state) {
		try {
			if (mState.exchange(state) != state)
				mStateChangeCallback(state);
		} catch (const std::exception &e) {
			PLOG_WARNING << e.what();
		}
	}

	virtual void incoming(message_ptr message) { recv(message); }
	virtual bool outgoing(message_ptr message) {
		if (mLower)
			return mLower->send(message);
		else
			return false;
	}

private:
	shared_ptr<Transport> mLower;
	synchronized_callback<State> mStateChangeCallback;
	synchronized_callback<message_ptr> mRecvCallback;

	std::atomic<State> mState = State::Disconnected;
	std::atomic<bool> mStopped = true;
};

} // namespace rtc::impl

#endif
