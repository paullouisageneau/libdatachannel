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
#include "queue.hpp"
#include "transport.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

#include "usrsctp.h"

namespace rtc {

class SctpTransport : public Transport {
public:
	static void Init();
	static void Cleanup();

	using amount_callback = std::function<void(uint16_t streamId, size_t amount)>;

	SctpTransport(std::shared_ptr<Transport> lower, uint16_t port, message_callback recvCallback,
	              amount_callback bufferedAmountCallback, state_callback stateChangeCallback);
	~SctpTransport();

	bool stop() override;
	bool send(message_ptr message) override; // false if buffered
	void close(unsigned int stream);
	void flush();

	// Stats
	void clearStats();
	size_t bytesSent();
	size_t bytesReceived();
	std::optional<std::chrono::milliseconds> rtt();

private:
	// Order seems wrong but these are the actual values
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13#section-8
	enum PayloadId : uint32_t {
		PPID_CONTROL = 50,
		PPID_STRING = 51,
		PPID_BINARY_PARTIAL = 52,
		PPID_BINARY = 53,
		PPID_STRING_PARTIAL = 54,
		PPID_STRING_EMPTY = 56,
		PPID_BINARY_EMPTY = 57
	};

	void connect();
	void shutdown();
	void incoming(message_ptr message) override;

	bool trySendQueue();
	bool trySendMessage(message_ptr message);
	void updateBufferedAmount(uint16_t streamId, long delta);
	void sendReset(uint16_t streamId);
	bool safeFlush();

	int handleRecv(struct socket *sock, union sctp_sockstore addr, const byte *data, size_t len,
	               struct sctp_rcvinfo recv_info, int flags);
	int handleSend(size_t free);
	int handleWrite(byte *data, size_t len, uint8_t tos, uint8_t set_df);

	void processData(const byte *data, size_t len, uint16_t streamId, PayloadId ppid);
	void processNotification(const union sctp_notification *notify, size_t len);

	const uint16_t mPort;
	struct socket *mSock;

	std::mutex mSendMutex;
	Queue<message_ptr> mSendQueue;
	std::map<uint16_t, size_t> mBufferedAmount;
	amount_callback mBufferedAmountCallback;

	std::mutex mWriteMutex;
	std::condition_variable mWrittenCondition;
	std::atomic<bool> mWritten = false; // written outside lock
	std::atomic<bool> mWrittenOnce = false; // same

	binary mPartialRecv, mPartialStringData, mPartialBinaryData;

	// Stats
	std::atomic<size_t> mBytesSent = 0, mBytesReceived = 0;

	static int RecvCallback(struct socket *sock, union sctp_sockstore addr, void *data, size_t len,
	                        struct sctp_rcvinfo recv_info, int flags, void *user_data);
	static int SendCallback(struct socket *sock, uint32_t sb_free);
	static int WriteCallback(void *sctp_ptr, void *data, size_t len, uint8_t tos, uint8_t set_df);
};

} // namespace rtc

#endif
