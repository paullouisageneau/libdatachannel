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

#include <functional>
#include <memory>

namespace rtc {

using namespace std::placeholders;

class Transport {
public:
	Transport(std::shared_ptr<Transport> lower = nullptr) : mLower(std::move(lower)) {
		if (mLower)
			mLower->onRecv(std::bind(&Transport::incoming, this, _1));
	}
	virtual ~Transport() {
		if (mLower)
			mLower->onRecv(nullptr);
	}

	virtual bool send(message_ptr message) = 0;
	void onRecv(message_callback callback) { mRecvCallback = std::move(callback); }

protected:
	void recv(message_ptr message) { mRecvCallback(message); }

	virtual void incoming(message_ptr message) = 0;
	virtual void outgoing(message_ptr message) { getLower()->send(message); }

private:
	std::shared_ptr<Transport> getLower() {
		if (mLower)
			return mLower;
		else
			throw std::logic_error("No lower transport to call");
	}

	std::shared_ptr<Transport> mLower;
	synchronized_callback<message_ptr> mRecvCallback;
};

} // namespace rtc

#endif
