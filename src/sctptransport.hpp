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

#ifndef RTC_SCTP_TRANSPORT_H
#define RTC_SCTP_TRANSPORT_H

#include "include.hpp"
#include "peerconnection.hpp"
#include "transport.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <sys/socket.h>
#include <sys/types.h>
#include <usrsctp.h>

namespace rtc {

class SctpTransport : public Transport {
public:
	enum class State { Disconnected, Connecting, Connected, Failed };

	using state_callback = std::function<void(State state)>;

	SctpTransport(std::shared_ptr<Transport> lower, uint16_t port, message_callback recv,
	              state_callback stateChangedCallback);
	~SctpTransport();

	State state() const;

	bool send(message_ptr message);
	void reset(unsigned int stream);

private:
	enum PayloadId : uint32_t {
		PPID_CONTROL = 50,
		PPID_STRING = 51,
		PPID_BINARY = 53,
		PPID_STRING_EMPTY = 56,
		PPID_BINARY_EMPTY = 57
	};

	void incoming(message_ptr message);
	void changeState(State state);
	void runConnect();

	int handleWrite(void *data, size_t len, uint8_t tos, uint8_t set_df);

	int process(struct socket *sock, union sctp_sockstore addr, void *data, size_t len,
	            struct sctp_rcvinfo recv_info, int flags);

	void processData(const byte *data, size_t len, uint16_t streamId, PayloadId ppid);
	void processNotification(const union sctp_notification *notify, size_t len);

	struct socket *mSock;
	uint16_t mPort;

	std::thread mConnectThread;
	std::mutex mConnectMutex;
	std::condition_variable mConnectCondition;
	std::atomic<bool> mConnectDataSent = false;
	std::atomic<bool> mStopping = false;

	std::atomic<State> mState;

	state_callback mStateChangedCallback;

	static int WriteCallback(void *sctp_ptr, void *data, size_t len, uint8_t tos, uint8_t set_df);
	static int ReadCallback(struct socket *sock, union sctp_sockstore addr, void *data, size_t len,
	                        struct sctp_rcvinfo recv_info, int flags, void *user_data);

	void GlobalInit();
	void GlobalCleanup();

	static std::mutex GlobalMutex;
	static int InstancesCount;
};

} // namespace rtc

#endif
