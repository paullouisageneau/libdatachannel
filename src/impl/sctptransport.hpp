/**
 * Copyright (c) 2019-2021 Paul-Louis Ageneau
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

#ifndef RTC_IMPL_SCTP_TRANSPORT_H
#define RTC_IMPL_SCTP_TRANSPORT_H

#include "common.hpp"
#include "configuration.hpp"
#include "processor.hpp"
#include "queue.hpp"
#include "transport.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

#include "usrsctp.h"

namespace rtc::impl {

class SctpTransport final : public Transport {
public:
	static void Init();
	static void SetSettings(const SctpSettings &s);
	static void Cleanup();

	using amount_callback = std::function<void(uint16_t streamId, size_t amount)>;

	SctpTransport(shared_ptr<Transport> lower, const Configuration &config, uint16_t port,
	              message_callback recvCallback, amount_callback bufferedAmountCallback,
	              state_callback stateChangeCallback);
	~SctpTransport();

	void start() override;
	bool stop() override;
	bool send(message_ptr message) override; // false if buffered
	bool flush();
	void closeStream(unsigned int stream);

	void onBufferedAmount(amount_callback callback) {
		mBufferedAmountCallback = std::move(callback);
	}

	// Stats
	void clearStats();
	size_t bytesSent();
	size_t bytesReceived();
	optional<std::chrono::milliseconds> rtt();

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
	void close();
	void incoming(message_ptr message) override;
	bool outgoing(message_ptr message) override;

	void doRecv();
	void doFlush();
	bool trySendQueue();
	bool trySendMessage(message_ptr message);
	void updateBufferedAmount(uint16_t streamId, ptrdiff_t delta);
	void triggerBufferedAmount(uint16_t streamId, size_t amount);
	void sendReset(uint16_t streamId);

	void handleUpcall();
	int handleWrite(byte *data, size_t len, uint8_t tos, uint8_t set_df);

	void processData(binary &&data, uint16_t streamId, PayloadId ppid);
	void processNotification(const union sctp_notification *notify, size_t len);

	const uint16_t mPort;
	struct socket *mSock;

	Processor mProcessor;
	std::atomic<int> mPendingRecvCount = 0;
	std::atomic<int> mPendingFlushCount = 0;
	std::mutex mRecvMutex;
	std::recursive_mutex mSendMutex; // buffered amount callback is synchronous
	Queue<message_ptr> mSendQueue;
	std::map<uint16_t, size_t> mBufferedAmount;
	amount_callback mBufferedAmountCallback;

	std::mutex mWriteMutex;
	std::condition_variable mWrittenCondition;
	std::atomic<bool> mWritten = false;     // written outside lock
	std::atomic<bool> mWrittenOnce = false; // same

	binary mPartialMessage, mPartialNotification;
	binary mPartialStringData, mPartialBinaryData;

	// Stats
	std::atomic<size_t> mBytesSent = 0, mBytesReceived = 0;

	static void UpcallCallback(struct socket *sock, void *arg, int flags);
	static int WriteCallback(void *sctp_ptr, void *data, size_t len, uint8_t tos, uint8_t set_df);
	static void DebugCallback(const char *format, ...);

	class InstancesSet;
	static InstancesSet *Instances;
};

} // namespace rtc::impl

#endif
