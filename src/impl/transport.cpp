/**
 * Copyright (c) 2019-2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "transport.hpp"

namespace rtc::impl {

Transport::Transport(shared_ptr<Transport> lower, state_callback callback)
    : mLower(std::move(lower)), mStateChangeCallback(std::move(callback)) {}

Transport::~Transport() {
	unregisterIncoming();

	if (mLower) {
		mLower->stop();
		mLower.reset();
	}
}

void Transport::registerIncoming() {
	if (mLower) {
		PLOG_VERBOSE << "Registering incoming callback";
		mLower->onRecv(std::bind(&Transport::incoming, this, std::placeholders::_1));
	}
}

void Transport::unregisterIncoming() {
	if (mLower) {
		PLOG_VERBOSE << "Unregistering incoming callback";
		mLower->onRecv(nullptr);
	}
}

Transport::State Transport::state() const { return mState; }

void Transport::onRecv(message_callback callback) {
	std::vector<message_ptr> pending;
	{
		std::lock_guard lock(mPendingMutex);
		mRecvCallback = std::move(callback);
		if (mRecvCallback)
			pending = std::move(mPendingRecv);
		else
			mPendingRecv.clear();
	}
	for (auto &msg : pending) {
		try {
			mRecvCallback(std::move(msg));
		} catch (const std::exception &e) {
			PLOG_WARNING << e.what();
		}
	}
}

void Transport::onStateChange(state_callback callback) {
	mStateChangeCallback = std::move(callback);
}

void Transport::start() { registerIncoming(); }

void Transport::stop() { unregisterIncoming(); }

bool Transport::send(message_ptr message) { return outgoing(std::move(message)); }

bool Transport::send(const byte *data, size_t size, unsigned int dscp) {
	if (!data && size > 0)
		return false;

	auto message = size > 0 ? make_message(data, data + size) : make_message(0);
	message->dscp = dscp;
	return send(std::move(message));
}

void Transport::recv(message_ptr message) {
	try {
		std::unique_lock lock(mPendingMutex);
		if (!mRecvCallback) {
			// No callback registered yet; buffer the message for replay when
			// onRecv() is called.  Bounded to avoid unbounded growth.
			if (mPendingRecv.size() < 8)
				mPendingRecv.push_back(std::move(message));
			else
				PLOG_WARNING << "dropping incoming message, no receive callback";
			return;
		}
		lock.unlock();
		mRecvCallback(std::move(message));
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

void Transport::incoming(message_ptr message) { recv(std::move(message)); }

bool Transport::outgoing(message_ptr message) {
	if (mLower)
		return mLower->send(std::move(message));
	else
		return false;
}

bool Transport::outgoing(const byte *data, size_t size, unsigned int dscp) {
	if (mLower)
		return mLower->send(data, size, dscp);
	else
		return false;
}

} // namespace rtc::impl
