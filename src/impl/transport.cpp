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

#include "transport.hpp"

namespace rtc::impl {

Transport::Transport(shared_ptr<Transport> lower, state_callback callback)
    : mLower(std::move(lower)), mStateChangeCallback(std::move(callback)) {}

Transport::~Transport() { stop(); }

void Transport::start() { mStopped = false; }

bool Transport::stop() {
	if (mStopped.exchange(true))
		return false;

	// We don't want incoming() to be called by the lower layer anymore
	if (mLower) {
		PLOG_VERBOSE << "Unregistering incoming callback";
		mLower->onRecv(nullptr);
	}
	return true;
}

void Transport::registerIncoming() {
	if (mLower) {
		PLOG_VERBOSE << "Registering incoming callback";
		mLower->onRecv(std::bind(&Transport::incoming, this, std::placeholders::_1));
	}
}

Transport::State Transport::state() const { return mState; }

void Transport::onRecv(message_callback callback) { mRecvCallback = std::move(callback); }

void Transport::onStateChange(state_callback callback) {
	mStateChangeCallback = std::move(callback);
}

bool Transport::send(message_ptr message) { return outgoing(message); }

void Transport::recv(message_ptr message) {
	try {
		mRecvCallback(message);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void Transport::changeState(State state) {
	try {
		if (mState.exchange(state) != state)
			mStateChangeCallback(state);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void Transport::incoming(message_ptr message) { recv(message); }

bool Transport::outgoing(message_ptr message) {
	if (mLower)
		return mLower->send(message);
	else
		return false;
}

} // namespace rtc::impl

