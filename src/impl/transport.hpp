/**
 * Copyright (c) 2019-2022 Paul-Louis Ageneau
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

	Transport(shared_ptr<Transport> lower = nullptr, state_callback callback = nullptr);
	virtual ~Transport();

	virtual void start();
	virtual bool stop();

	void registerIncoming();
	State state() const;

	void onRecv(message_callback callback);
	void onStateChange(state_callback callback);

	virtual bool send(message_ptr message);

protected:
	void recv(message_ptr message);
	void changeState(State state);
	virtual void incoming(message_ptr message);
	virtual bool outgoing(message_ptr message);

private:
	const shared_ptr<Transport> mLower;
	synchronized_callback<State> mStateChangeCallback;
	synchronized_callback<message_ptr> mRecvCallback;

	std::atomic<State> mState = State::Disconnected;
	std::atomic<bool> mStopped = true;
};

} // namespace rtc::impl

#endif
