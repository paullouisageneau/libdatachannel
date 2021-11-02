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

#include "sctptransport.hpp"
#include "dtlstransport.hpp"
#include "internals.hpp"
#include "logcounter.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <vector>

// RFC 8831: SCTP MUST support performing Path MTU discovery without relying on ICMP or ICMPv6 as
// specified in [RFC4821] by using probing messages specified in [RFC4820].
// See https://tools.ietf.org/html/rfc8831#section-5
//
// However, usrsctp does not implement Path MTU discovery, so we need to disable it for now.
// See https://github.com/sctplab/usrsctp/issues/205
#define USE_PMTUD 0

// TODO: When Path MTU discovery is supported, it needs to be enabled with libjuice as ICE backend
// on all platforms except Mac OS where the Don't Fragment (DF) flag can't be set:
/*
#if !USE_NICE
#ifndef __APPLE__
// libjuice enables Linux path MTU discovery or sets the DF flag
#define USE_PMTUD 1
#else
// Setting the DF flag is not available on Mac OS
#define USE_PMTUD 0
#endif
#else // USE_NICE == 1
#define USE_PMTUD 0
#endif
*/

using namespace std::chrono_literals;
using namespace std::chrono;

namespace {

template <typename T> uint16_t to_uint16(T i) {
	if (i >= 0 && static_cast<typename std::make_unsigned<T>::type>(i) <=
	                  std::numeric_limits<uint16_t>::max())
		return static_cast<uint16_t>(i);
	else
		throw std::invalid_argument("Integer out of range");
}

template <typename T> uint32_t to_uint32(T i) {
	if (i >= 0 && static_cast<typename std::make_unsigned<T>::type>(i) <=
	                  std::numeric_limits<uint32_t>::max())
		return static_cast<uint32_t>(i);
	else
		throw std::invalid_argument("Integer out of range");
}

} // namespace

namespace rtc::impl {

static LogCounter COUNTER_UNKNOWN_PPID(plog::warning,
                                       "Number of SCTP packets received with an unknown PPID");
static LogCounter
    COUNTER_BAD_NOTIF_LEN(plog::warning,
                          "Number of SCTP packets received with an bad notification length");
static LogCounter COUNTER_BAD_SCTP_STATUS(plog::warning,
                                          "Number of SCTP packets received with a bad status");

class SctpTransport::InstancesSet {
public:
	void insert(SctpTransport *instance) {
		std::unique_lock lock(mMutex);
		mSet.insert(instance);
	}

	void erase(SctpTransport *instance) {
		std::unique_lock lock(mMutex);
		mSet.erase(instance);
	}

	using shared_lock = std::shared_lock<std::shared_mutex>;
	optional<shared_lock> lock(SctpTransport *instance) {
		shared_lock lock(mMutex);
		return mSet.find(instance) != mSet.end() ? std::make_optional(std::move(lock)) : nullopt;
	}

private:
	std::unordered_set<SctpTransport *> mSet;
	std::shared_mutex mMutex;
};

SctpTransport::InstancesSet *SctpTransport::Instances = new InstancesSet;

void SctpTransport::Init() {
	usrsctp_init(0, SctpTransport::WriteCallback, SctpTransport::DebugCallback);
	usrsctp_enable_crc32c_offload();       // We'll compute CRC32 only for outgoing packets
	usrsctp_sysctl_set_sctp_pr_enable(1);  // Enable Partial Reliability Extension (RFC 3758)
	usrsctp_sysctl_set_sctp_ecn_enable(0); // Disable Explicit Congestion Notification
#ifdef SCTP_DEBUG
	usrsctp_sysctl_set_sctp_debug_on(SCTP_DEBUG_ALL);
#endif
}

void SctpTransport::SetSettings(const SctpSettings &s) {
	// The send and receive window size of usrsctp is 256KiB, which is too small for realistic RTTs,
	// therefore we increase it to 1MiB by default for better performance.
	// See https://bugzilla.mozilla.org/show_bug.cgi?id=1051685
	usrsctp_sysctl_set_sctp_recvspace(to_uint32(s.recvBufferSize.value_or(1024 * 1024)));
	usrsctp_sysctl_set_sctp_sendspace(to_uint32(s.sendBufferSize.value_or(1024 * 1024)));

	// Increase maximum chunks number on queue to 10K by default
	usrsctp_sysctl_set_sctp_max_chunks_on_queue(to_uint32(s.maxChunksOnQueue.value_or(10 * 1024)));

	// Increase initial congestion window size to 10 MTUs (RFC 6928) by default
	usrsctp_sysctl_set_sctp_initial_cwnd(to_uint32(s.initialCongestionWindow.value_or(10)));

	// Set max burst to 10 MTUs by default (max burst is initially 0, meaning disabled)
	usrsctp_sysctl_set_sctp_max_burst_default(to_uint32(s.maxBurst.value_or(10)));

	// Use standard SCTP congestion control (RFC 4960) by default
	// See https://github.com/paullouisageneau/libdatachannel/issues/354
	usrsctp_sysctl_set_sctp_default_cc_module(to_uint32(s.congestionControlModule.value_or(0)));

	// Reduce SACK delay to 20ms by default (the recommended default value from RFC 4960 is 200ms)
	usrsctp_sysctl_set_sctp_delayed_sack_time_default(
	    to_uint32(s.delayedSackTime.value_or(20ms).count()));

	// RTO settings
	// RFC 2988 recommends a 1s min RTO, which is very high, but TCP on Linux has a 200ms min RTO
	usrsctp_sysctl_set_sctp_rto_min_default(
	    to_uint32(s.minRetransmitTimeout.value_or(200ms).count()));
	// Set only 10s as max RTO instead of 60s for shorter connection timeout
	usrsctp_sysctl_set_sctp_rto_max_default(
	    to_uint32(s.maxRetransmitTimeout.value_or(10000ms).count()));
	usrsctp_sysctl_set_sctp_init_rto_max_default(
	    to_uint32(s.maxRetransmitTimeout.value_or(10000ms).count()));
	// Still set 1s as initial RTO
	usrsctp_sysctl_set_sctp_rto_initial_default(
	    to_uint32(s.initialRetransmitTimeout.value_or(1000ms).count()));

	// RTX settings
	// 5 retransmissions instead of 8 to shorten the backoff for shorter connection timeout
	auto maxRtx = to_uint32(s.maxRetransmitAttempts.value_or(5));
	usrsctp_sysctl_set_sctp_init_rtx_max_default(maxRtx);
	usrsctp_sysctl_set_sctp_assoc_rtx_max_default(maxRtx);
	usrsctp_sysctl_set_sctp_path_rtx_max_default(maxRtx); // single path

	// Heartbeat interval
	usrsctp_sysctl_set_sctp_heartbeat_interval_default(
	    to_uint32(s.heartbeatInterval.value_or(10000ms).count()));
}

void SctpTransport::Cleanup() {
	while (usrsctp_finish() != 0)
		std::this_thread::sleep_for(100ms);
}

SctpTransport::SctpTransport(shared_ptr<Transport> lower, const Configuration &config,
                             uint16_t port, message_callback recvCallback,
                             amount_callback bufferedAmountCallback,
                             state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mPort(port),
      mSendQueue(0, message_size_func), mBufferedAmountCallback(std::move(bufferedAmountCallback)) {
	onRecv(std::move(recvCallback));

	PLOG_DEBUG << "Initializing SCTP transport";

	usrsctp_register_address(this);
	Instances->insert(this);

	mSock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, nullptr, nullptr, 0, nullptr);
	if (!mSock)
		throw std::runtime_error("Could not create SCTP socket, errno=" + std::to_string(errno));

	usrsctp_set_upcall(mSock, &SctpTransport::UpcallCallback, this);

	if (usrsctp_set_non_blocking(mSock, 1))
		throw std::runtime_error("Unable to set non-blocking mode, errno=" + std::to_string(errno));

	// SCTP must stop sending after the lower layer is shut down, so disable linger
	struct linger sol = {};
	sol.l_onoff = 1;
	sol.l_linger = 0;
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_LINGER, &sol, sizeof(sol)))
		throw std::runtime_error("Could not set socket option SO_LINGER, errno=" +
		                         std::to_string(errno));

	struct sctp_assoc_value av = {};
	av.assoc_id = SCTP_ALL_ASSOC;
	av.assoc_value = 1;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av, sizeof(av)))
		throw std::runtime_error("Could not set socket option SCTP_ENABLE_STREAM_RESET, errno=" +
		                         std::to_string(errno));
	int on = 1;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on)))
		throw std::runtime_error("Could set socket option SCTP_RECVRCVINFO, errno=" +
		                         std::to_string(errno));

	struct sctp_event se = {};
	se.se_assoc_id = SCTP_ALL_ASSOC;
	se.se_on = 1;
	se.se_type = SCTP_ASSOC_CHANGE;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_EVENT, &se, sizeof(se)))
		throw std::runtime_error("Could not subscribe to event SCTP_ASSOC_CHANGE, errno=" +
		                         std::to_string(errno));
	se.se_type = SCTP_SENDER_DRY_EVENT;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_EVENT, &se, sizeof(se)))
		throw std::runtime_error("Could not subscribe to event SCTP_SENDER_DRY_EVENT, errno=" +
		                         std::to_string(errno));
	se.se_type = SCTP_STREAM_RESET_EVENT;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_EVENT, &se, sizeof(se)))
		throw std::runtime_error("Could not subscribe to event SCTP_STREAM_RESET_EVENT, errno=" +
		                         std::to_string(errno));

	// RFC 8831 6.6. Transferring User Data on a Data Channel
	// The sender SHOULD disable the Nagle algorithm (see [RFC1122) to minimize the latency
	// See https://tools.ietf.org/html/rfc8831#section-6.6
	int nodelay = 1;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay)))
		throw std::runtime_error("Could not set socket option SCTP_NODELAY, errno=" +
		                         std::to_string(errno));

	struct sctp_paddrparams spp = {};
	// Enable SCTP heartbeats
	spp.spp_flags = SPP_HB_ENABLE;

	// RFC 8261 5. DTLS considerations:
	// If path MTU discovery is performed by the SCTP layer and IPv4 is used as the network-layer
	// protocol, the DTLS implementation SHOULD allow the DTLS user to enforce that the
	// corresponding IPv4 packet is sent with the Don't Fragment (DF) bit set. If controlling the DF
	// bit is not possible (for example, due to implementation restrictions), a safe value for the
	// path MTU has to be used by the SCTP stack. It is RECOMMENDED that the safe value not exceed
	// 1200 bytes.
	// See https://tools.ietf.org/html/rfc8261#section-5
#if USE_PMTUD
	if (!config.mtu.has_value()) {
#else
	if (false) {
#endif
		// Enable SCTP path MTU discovery
		spp.spp_flags |= SPP_PMTUD_ENABLE;
		PLOG_VERBOSE << "Path MTU discovery enabled";

	} else {
		// Fall back to a safe MTU value.
		spp.spp_flags |= SPP_PMTUD_DISABLE;
		// The MTU value provided specifies the space available for chunks in the
		// packet, so we also subtract the SCTP header size.
		size_t pmtu = config.mtu.value_or(DEFAULT_MTU) - 12 - 48 - 8 - 40; // SCTP/DTLS/UDP/IPv6
		spp.spp_pathmtu = to_uint32(pmtu);
		PLOG_VERBOSE << "Path MTU discovery disabled, SCTP MTU set to " << pmtu;
	}

	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &spp, sizeof(spp)))
		throw std::runtime_error("Could not set socket option SCTP_PEER_ADDR_PARAMS, errno=" +
		                         std::to_string(errno));

	// RFC 8831 6.2. SCTP Association Management
	// The number of streams negotiated during SCTP association setup SHOULD be 65535, which is the
	// maximum number of streams that can be negotiated during the association setup.
	// See https://tools.ietf.org/html/rfc8831#section-6.2
	struct sctp_initmsg sinit = {};
	sinit.sinit_num_ostreams = 65535;
	sinit.sinit_max_instreams = 65535;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_INITMSG, &sinit, sizeof(sinit)))
		throw std::runtime_error("Could not set socket option SCTP_INITMSG, errno=" +
		                         std::to_string(errno));

	// Prevent fragmented interleave of messages (i.e. level 0), see RFC 6458 section 8.1.20.
	// Unless the user has set the fragmentation interleave level to 0, notifications
	// may also be interleaved with partially delivered messages.
	int level = 0;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE, &level, sizeof(level)))
		throw std::runtime_error("Could not disable SCTP fragmented interleave, errno=" +
		                         std::to_string(errno));

	int rcvBuf = 0;
	socklen_t rcvBufLen = sizeof(rcvBuf);
	if (usrsctp_getsockopt(mSock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, &rcvBufLen))
		throw std::runtime_error("Could not get SCTP recv buffer size, errno=" +
		                         std::to_string(errno));
	int sndBuf = 0;
	socklen_t sndBufLen = sizeof(sndBuf);
	if (usrsctp_getsockopt(mSock, SOL_SOCKET, SO_SNDBUF, &sndBuf, &sndBufLen))
		throw std::runtime_error("Could not get SCTP send buffer size, errno=" +
		                         std::to_string(errno));

	// Ensure the buffer is also large enough to accomodate the largest messages
	const size_t maxMessageSize = config.maxMessageSize.value_or(DEFAULT_LOCAL_MAX_MESSAGE_SIZE);
	const int minBuf = int(std::min(maxMessageSize, size_t(std::numeric_limits<int>::max())));
	rcvBuf = std::max(rcvBuf, minBuf);
	sndBuf = std::max(sndBuf, minBuf);

	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf)))
		throw std::runtime_error("Could not set SCTP recv buffer size, errno=" +
		                         std::to_string(errno));

	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf)))
		throw std::runtime_error("Could not set SCTP send buffer size, errno=" +
		                         std::to_string(errno));
}

SctpTransport::~SctpTransport() {
	stop();
	close();

	usrsctp_deregister_address(this);
	Instances->erase(this);
}

void SctpTransport::start() {
	Transport::start();

	registerIncoming();
	connect();
}

bool SctpTransport::stop() {
	// Transport::stop() will unregister incoming() from the lower layer, therefore we need to make
	// sure the thread from lower layers is not blocked in incoming() by the WrittenOnce condition.
	mWrittenOnce = true;
	mWrittenCondition.notify_all();

	if (!Transport::stop())
		return false;

	mSendQueue.stop();
	flush();
	shutdown();
	return true;
}

void SctpTransport::close() {
	if (mSock) {
		mProcessor.join();
		usrsctp_close(mSock);
		mSock = nullptr;
	}
}

void SctpTransport::connect() {
	if (!mSock)
		throw std::logic_error("Attempted SCTP connect with closed socket");

	PLOG_DEBUG << "SCTP connecting";
	changeState(State::Connecting);

	struct sockaddr_conn sconn = {};
	sconn.sconn_family = AF_CONN;
	sconn.sconn_port = htons(mPort);
	sconn.sconn_addr = this;
#ifdef HAVE_SCONN_LEN
	sconn.sconn_len = sizeof(sconn);
#endif

	if (usrsctp_bind(mSock, reinterpret_cast<struct sockaddr *>(&sconn), sizeof(sconn)))
		throw std::runtime_error("Could not bind usrsctp socket, errno=" + std::to_string(errno));

	// According to RFC 8841, both endpoints must initiate the SCTP association, in a
	// simultaneous-open manner, irrelevent to the SDP setup role.
	// See https://tools.ietf.org/html/rfc8841#section-9.3
	int ret = usrsctp_connect(mSock, reinterpret_cast<struct sockaddr *>(&sconn), sizeof(sconn));
	if (ret && errno != EINPROGRESS)
		throw std::runtime_error("Connection attempt failed, errno=" + std::to_string(errno));
}

void SctpTransport::shutdown() {
	if (!mSock)
		return;

	PLOG_DEBUG << "SCTP shutdown";

	if (usrsctp_shutdown(mSock, SHUT_RDWR) != 0 && errno != ENOTCONN) {
		PLOG_WARNING << "SCTP shutdown failed, errno=" << errno;
	}

	close();

	PLOG_INFO << "SCTP disconnected";
	changeState(State::Disconnected);
	mWrittenCondition.notify_all();
}

bool SctpTransport::send(message_ptr message) {
	std::lock_guard lock(mSendMutex);

	if (!message)
		return trySendQueue();

	PLOG_VERBOSE << "Send size=" << message->size();

	// Flush the queue, and if nothing is pending, try to send directly
	if (trySendQueue() && trySendMessage(message))
		return true;

	mSendQueue.push(message);
	updateBufferedAmount(to_uint16(message->stream), ptrdiff_t(message_size_func(message)));
	return false;
}

bool SctpTransport::flush() {
	try {
		std::lock_guard lock(mSendMutex);
		trySendQueue();
		return true;

	} catch (const std::exception &e) {
		PLOG_WARNING << "SCTP flush: " << e.what();
		return false;
	}
}

void SctpTransport::closeStream(unsigned int stream) {
	std::lock_guard lock(mSendMutex);

	// This method must not call the buffered callback synchronously
	mSendQueue.push(make_message(0, Message::Reset, to_uint16(stream)));
	mProcessor.enqueue(&SctpTransport::flush, this);
}

void SctpTransport::incoming(message_ptr message) {
	// There could be a race condition here where we receive the remote INIT before the local one is
	// sent, which would result in the connection being aborted. Therefore, we need to wait for data
	// to be sent on our side (i.e. the local INIT) before proceeding.
	if (!mWrittenOnce) { // test the atomic boolean is not set first to prevent a lock contention
		std::unique_lock lock(mWriteMutex);
		mWrittenCondition.wait(lock, [&]() { return mWrittenOnce.load(); });
	}

	if (!message) {
		PLOG_INFO << "SCTP disconnected";
		changeState(State::Disconnected);
		recv(nullptr);
		return;
	}

	PLOG_VERBOSE << "Incoming size=" << message->size();

	usrsctp_conninput(this, message->data(), message->size(), 0);
}

bool SctpTransport::outgoing(message_ptr message) {
	// Set recommended medium-priority DSCP value
	// See https://datatracker.ietf.org/doc/html/rfc8837#section-5
	message->dscp = 10; // AF11: Assured Forwarding class 1, low drop probability
	return Transport::outgoing(std::move(message));
}

void SctpTransport::doRecv() {
	std::lock_guard lock(mRecvMutex);
	--mPendingRecvCount;
	try {
		while (state() != State::Disconnected && state() != State::Failed) {
			const size_t bufferSize = 65536;
			byte buffer[bufferSize];
			socklen_t fromlen = 0;
			struct sctp_rcvinfo info = {};
			socklen_t infolen = sizeof(info);
			unsigned int infotype = 0;
			int flags = 0;
			ssize_t len = usrsctp_recvv(mSock, buffer, bufferSize, nullptr, &fromlen, &info,
			                            &infolen, &infotype, &flags);
			if (len < 0) {
				if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ECONNRESET)
					break;
				else
					throw std::runtime_error("SCTP recv failed, errno=" + std::to_string(errno));
			}

			PLOG_VERBOSE << "SCTP recv, len=" << len;

			// SCTP_FRAGMENT_INTERLEAVE does not seem to work as expected for messages > 64KB,
			// therefore partial notifications and messages need to be handled separately.
			if (flags & MSG_NOTIFICATION) {
				// SCTP event notification
				mPartialNotification.insert(mPartialNotification.end(), buffer, buffer + len);
				if (flags & MSG_EOR) {
					// Notification is complete, process it
					auto notification =
					    reinterpret_cast<union sctp_notification *>(mPartialNotification.data());
					processNotification(notification, mPartialNotification.size());
					mPartialNotification.clear();
				}
			} else {
				// SCTP message
				mPartialMessage.insert(mPartialMessage.end(), buffer, buffer + len);
				if (flags & MSG_EOR) {
					// Message is complete, process it
					if (infotype != SCTP_RECVV_RCVINFO)
						throw std::runtime_error("Missing SCTP recv info");

					processData(std::move(mPartialMessage), info.rcv_sid,
					            PayloadId(ntohl(info.rcv_ppid)));
					mPartialMessage.clear();
				}
			}
		}
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void SctpTransport::doFlush() {
	std::lock_guard lock(mSendMutex);
	--mPendingFlushCount;
	try {
		trySendQueue();
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

bool SctpTransport::trySendQueue() {
	// Requires mSendMutex to be locked
	while (auto next = mSendQueue.peek()) {
		message_ptr message = std::move(*next);
		if (!trySendMessage(message))
			return false;

		mSendQueue.pop();
		updateBufferedAmount(to_uint16(message->stream), -ptrdiff_t(message_size_func(message)));
	}
	return true;
}

bool SctpTransport::trySendMessage(message_ptr message) {
	// Requires mSendMutex to be locked
	if (!mSock || state() != State::Connected)
		return false;

	uint32_t ppid;
	switch (message->type) {
	case Message::String:
		ppid = !message->empty() ? PPID_STRING : PPID_STRING_EMPTY;
		break;
	case Message::Binary:
		ppid = !message->empty() ? PPID_BINARY : PPID_BINARY_EMPTY;
		break;
	case Message::Control:
		ppid = PPID_CONTROL;
		break;
	case Message::Reset:
		sendReset(uint16_t(message->stream));
		return true;
	default:
		// Ignore
		return true;
	}

	PLOG_VERBOSE << "SCTP try send size=" << message->size();

	// TODO: Implement SCTP ndata specification draft when supported everywhere
	// See https://tools.ietf.org/html/draft-ietf-tsvwg-sctp-ndata-08

	const Reliability reliability = message->reliability ? *message->reliability : Reliability();

	struct sctp_sendv_spa spa = {};

	// set sndinfo
	spa.sendv_flags |= SCTP_SEND_SNDINFO_VALID;
	spa.sendv_sndinfo.snd_sid = uint16_t(message->stream);
	spa.sendv_sndinfo.snd_ppid = htonl(ppid);
	spa.sendv_sndinfo.snd_flags |= SCTP_EOR; // implicit here

	// set prinfo
	spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
	if (reliability.unordered)
		spa.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;

	switch (reliability.type) {
	case Reliability::Type::Rexmit:
		spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
		spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
		spa.sendv_prinfo.pr_value = to_uint32(std::get<int>(reliability.rexmit));
		break;
	case Reliability::Type::Timed:
		spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
		spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
		spa.sendv_prinfo.pr_value = to_uint32(std::get<milliseconds>(reliability.rexmit).count());
		break;
	default:
		spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_NONE;
		break;
	}

	ssize_t ret;
	if (!message->empty()) {
		ret = usrsctp_sendv(mSock, message->data(), message->size(), nullptr, 0, &spa, sizeof(spa),
		                    SCTP_SENDV_SPA, 0);
	} else {
		const char zero = 0;
		ret = usrsctp_sendv(mSock, &zero, 1, nullptr, 0, &spa, sizeof(spa), SCTP_SENDV_SPA, 0);
	}

	if (ret < 0) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			PLOG_VERBOSE << "SCTP sending not possible";
			return false;
		}

		PLOG_ERROR << "SCTP sending failed, errno=" << errno;
		throw std::runtime_error("Sending failed, errno=" + std::to_string(errno));
	}

	PLOG_VERBOSE << "SCTP sent size=" << message->size();
	if (message->type == Message::Binary || message->type == Message::String)
		mBytesSent += message->size();
	return true;
}

void SctpTransport::updateBufferedAmount(uint16_t streamId, ptrdiff_t delta) {
	// Requires mSendMutex to be locked

	if (delta == 0)
		return;

	auto it = mBufferedAmount.insert(std::make_pair(streamId, 0)).first;
	size_t amount = size_t(std::max(ptrdiff_t(it->second) + delta, ptrdiff_t(0)));
	if (amount == 0)
		mBufferedAmount.erase(it);
	else
		it->second = amount;

	// Synchronously call the buffered amount callback
	triggerBufferedAmount(streamId, amount);
}

void SctpTransport::triggerBufferedAmount(uint16_t streamId, size_t amount) {
	try {
		mBufferedAmountCallback(streamId, amount);
	} catch (const std::exception &e) {
		PLOG_WARNING << "SCTP buffered amount callback: " << e.what();
	}
}

void SctpTransport::sendReset(uint16_t streamId) {
	// Requires mSendMutex to be locked
	if (!mSock || state() != State::Connected)
		return;

	PLOG_DEBUG << "SCTP resetting stream " << streamId;

	using srs_t = struct sctp_reset_streams;
	const size_t len = sizeof(srs_t) + sizeof(uint16_t);
	byte buffer[len] = {};
	srs_t &srs = *reinterpret_cast<srs_t *>(buffer);
	srs.srs_flags = SCTP_STREAM_RESET_OUTGOING;
	srs.srs_number_streams = 1;
	srs.srs_stream_list[0] = streamId;

	mWritten = false;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_RESET_STREAMS, &srs, len) == 0) {
		std::unique_lock lock(mWriteMutex); // locking before setsockopt might deadlock usrsctp...
		mWrittenCondition.wait_for(lock, 1000ms,
		                           [&]() { return mWritten || state() != State::Connected; });
	} else if (errno == EINVAL) {
		PLOG_DEBUG << "SCTP stream " << streamId << " already reset";
	} else {
		PLOG_WARNING << "SCTP reset stream " << streamId << " failed, errno=" << errno;
	}
}

void SctpTransport::handleUpcall() {
	if (!mSock)
		return;

	PLOG_VERBOSE << "Handle upcall";

	int events = usrsctp_get_events(mSock);

	if (events & SCTP_EVENT_READ && mPendingRecvCount == 0) {
		++mPendingRecvCount;
		mProcessor.enqueue(&SctpTransport::doRecv, this);
	}

	if (events & SCTP_EVENT_WRITE && mPendingFlushCount == 0) {
		++mPendingFlushCount;
		mProcessor.enqueue(&SctpTransport::doFlush, this);
	}
}

int SctpTransport::handleWrite(byte *data, size_t len, uint8_t /*tos*/, uint8_t /*set_df*/) {
	try {
		std::unique_lock lock(mWriteMutex);
		PLOG_VERBOSE << "Handle write, len=" << len;

		if (!outgoing(make_message(data, data + len)))
			return -1;

		mWritten = true;
		mWrittenOnce = true;
		mWrittenCondition.notify_all();

	} catch (const std::exception &e) {
		PLOG_ERROR << "SCTP write: " << e.what();
		return -1;
	}
	return 0; // success
}

void SctpTransport::processData(binary &&data, uint16_t sid, PayloadId ppid) {
	PLOG_VERBOSE << "Process data, size=" << data.size();

	// RFC 8831: The usage of the PPIDs "WebRTC String Partial" and "WebRTC Binary Partial" is
	// deprecated. They were used for a PPID-based fragmentation and reassembly of user messages
	// belonging to reliable and ordered data channels.
	// See https://tools.ietf.org/html/rfc8831#section-6.6
	// We handle those PPIDs at reception for compatibility reasons but shall never send them.
	switch (ppid) {
	case PPID_CONTROL:
		recv(make_message(std::move(data), Message::Control, sid));
		break;

	case PPID_STRING_PARTIAL: // deprecated
		mPartialStringData.insert(mPartialStringData.end(), data.begin(), data.end());
		break;

	case PPID_STRING:
		if (mPartialStringData.empty()) {
			mBytesReceived += data.size();
			recv(make_message(std::move(data), Message::String, sid));
		} else {
			mPartialStringData.insert(mPartialStringData.end(), data.begin(), data.end());
			mBytesReceived += mPartialStringData.size();
			recv(make_message(std::move(mPartialStringData), Message::String, sid));
			mPartialStringData.clear();
		}
		break;

	case PPID_STRING_EMPTY:
		recv(make_message(std::move(mPartialStringData), Message::String, sid));
		mPartialStringData.clear();
		break;

	case PPID_BINARY_PARTIAL: // deprecated
		mPartialBinaryData.insert(mPartialBinaryData.end(), data.begin(), data.end());
		break;

	case PPID_BINARY:
		if (mPartialBinaryData.empty()) {
			mBytesReceived += data.size();
			recv(make_message(std::move(data), Message::Binary, sid));
		} else {
			mPartialBinaryData.insert(mPartialBinaryData.end(), data.begin(), data.end());
			mBytesReceived += mPartialBinaryData.size();
			recv(make_message(std::move(mPartialBinaryData), Message::Binary, sid));
			mPartialBinaryData.clear();
		}
		break;

	case PPID_BINARY_EMPTY:
		recv(make_message(std::move(mPartialBinaryData), Message::Binary, sid));
		mPartialBinaryData.clear();
		break;

	default:
		// Unknown
		COUNTER_UNKNOWN_PPID++;
		PLOG_VERBOSE << "Unknown PPID: " << uint32_t(ppid);
		return;
	}
}

void SctpTransport::processNotification(const union sctp_notification *notify, size_t len) {
	if (len != size_t(notify->sn_header.sn_length)) {
		COUNTER_BAD_NOTIF_LEN++;
		return;
	}

	auto type = notify->sn_header.sn_type;
	PLOG_VERBOSE << "Processing notification, type=" << type;

	switch (type) {
	case SCTP_ASSOC_CHANGE: {
		const struct sctp_assoc_change &assoc_change = notify->sn_assoc_change;
		if (assoc_change.sac_state == SCTP_COMM_UP) {
			PLOG_INFO << "SCTP connected";
			changeState(State::Connected);
		} else {
			if (state() == State::Connecting) {
				PLOG_ERROR << "SCTP connection failed";
				changeState(State::Failed);
			} else {
				PLOG_INFO << "SCTP disconnected";
				changeState(State::Disconnected);
			}
			mWrittenCondition.notify_all();
		}
		break;
	}

	case SCTP_SENDER_DRY_EVENT: {
		PLOG_VERBOSE << "SCTP dry event";
		// It should not be necessary since the send callback should have been called already,
		// but to be sure, let's try to send now.
		flush();
		break;
	}

	case SCTP_STREAM_RESET_EVENT: {
		const struct sctp_stream_reset_event &reset_event = notify->sn_strreset_event;
		const int count = (reset_event.strreset_length - sizeof(reset_event)) / sizeof(uint16_t);
		const uint16_t flags = reset_event.strreset_flags;

		IF_PLOG(plog::verbose) {
			std::ostringstream desc;
			desc << "flags=";
			if (flags & SCTP_STREAM_RESET_OUTGOING_SSN && flags & SCTP_STREAM_RESET_INCOMING_SSN)
				desc << "outgoing|incoming";
			else if (flags & SCTP_STREAM_RESET_OUTGOING_SSN)
				desc << "outgoing";
			else if (flags & SCTP_STREAM_RESET_INCOMING_SSN)
				desc << "incoming";
			else
				desc << "0";

			desc << ", streams=[";
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event.strreset_stream_list[i];
				desc << (i != 0 ? "," : "") << streamId;
			}
			desc << "]";

			PLOG_VERBOSE << "SCTP reset event, " << desc.str();
		}

		if (flags & SCTP_STREAM_RESET_OUTGOING_SSN) {
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event.strreset_stream_list[i];
				closeStream(streamId);
			}
		}
		if (flags & SCTP_STREAM_RESET_INCOMING_SSN) {
			const byte dataChannelCloseMessage{0x04};
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event.strreset_stream_list[i];
				recv(make_message(&dataChannelCloseMessage, &dataChannelCloseMessage + 1,
				                  Message::Control, streamId));
			}
		}
		break;
	}

	default:
		// Ignore
		break;
	}
}

void SctpTransport::clearStats() {
	mBytesReceived = 0;
	mBytesSent = 0;
}

size_t SctpTransport::bytesSent() { return mBytesSent; }

size_t SctpTransport::bytesReceived() { return mBytesReceived; }

optional<milliseconds> SctpTransport::rtt() {
	if (!mSock || state() != State::Connected)
		return nullopt;

	struct sctp_status status = {};
	socklen_t len = sizeof(status);
	if (usrsctp_getsockopt(mSock, IPPROTO_SCTP, SCTP_STATUS, &status, &len)) {
		COUNTER_BAD_SCTP_STATUS++;

		return nullopt;
	}
	return milliseconds(status.sstat_primary.spinfo_srtt);
}

void SctpTransport::UpcallCallback(struct socket *, void *arg, int /* flags */) {
	auto *transport = static_cast<SctpTransport *>(arg);

	if (auto locked = Instances->lock(transport))
		transport->handleUpcall();
}

int SctpTransport::WriteCallback(void *ptr, void *data, size_t len, uint8_t tos, uint8_t set_df) {
	auto *transport = static_cast<SctpTransport *>(ptr);

	// Set the CRC32 ourselves as we have enabled CRC32 offloading
	if (len >= 12) {
		uint32_t *checksum = reinterpret_cast<uint32_t *>(data) + 2;
		*checksum = 0;
		*checksum = usrsctp_crc32c(data, len);
	}

	// Workaround for sctplab/usrsctp#405: Send callback is invoked on already closed socket
	// https://github.com/sctplab/usrsctp/issues/405
	if (auto locked = Instances->lock(transport))
		return transport->handleWrite(static_cast<byte *>(data), len, tos, set_df);
	else
		return -1;
}

void SctpTransport::DebugCallback(const char *format, ...) {
	const size_t bufferSize = 1024;
	char buffer[bufferSize];
	va_list va;
	va_start(va, format);
	int len = std::vsnprintf(buffer, bufferSize, format, va);
	va_end(va);
	if (len <= 0)
		return;

	len = std::min(len, int(bufferSize - 1));
	buffer[len - 1] = '\0'; // remove newline

	PLOG_VERBOSE << "usrsctp: " << buffer; // usrsctp debug as verbose
}

} // namespace rtc::impl
