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

#include <chrono>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

#ifdef USE_JUICE
#ifndef __APPLE__
// libjuice enables Linux path MTU discovery or sets the DF flag
#define USE_PMTUD 1
#else
// Setting the DF flag is not available on Mac OS
#define USE_PMTUD 0
#endif
#else
#ifdef __linux__
// Linux UDP does path MTU discovery by default (setting DF and returning EMSGSIZE)
// It should be safe to enable discovery for SCTP.
#define USE_PMTUD 1
#else
// Otherwise assume fragmentation
#define USE_PMTUD 0
#endif
#endif

using namespace std::chrono_literals;
using namespace std::chrono;

using std::shared_ptr;

namespace rtc {

void SctpTransport::Init() {
	usrsctp_init(0, &SctpTransport::WriteCallback, nullptr);
	usrsctp_sysctl_set_sctp_ecn_enable(0);
	usrsctp_sysctl_set_sctp_init_rtx_max_default(5);
	usrsctp_sysctl_set_sctp_path_rtx_max_default(5);
	usrsctp_sysctl_set_sctp_assoc_rtx_max_default(5);              // single path
	usrsctp_sysctl_set_sctp_rto_min_default(1 * 1000);             // ms
	usrsctp_sysctl_set_sctp_rto_max_default(10 * 1000);            // ms
	usrsctp_sysctl_set_sctp_rto_initial_default(1 * 1000);         // ms
	usrsctp_sysctl_set_sctp_init_rto_max_default(10 * 1000);       // ms
	usrsctp_sysctl_set_sctp_heartbeat_interval_default(10 * 1000); // ms

	usrsctp_sysctl_set_sctp_max_chunks_on_queue(10 * 1024);

	// Change congestion control from the default TCP Reno (RFC 2581) to H-TCP
	usrsctp_sysctl_set_sctp_default_cc_module(SCTP_CC_HTCP);

	// Enable Non-Renegable Selective Acknowledgments (NR-SACKs)
	usrsctp_sysctl_set_sctp_nrsack_enable(1);

	// Increase the initial window size to 10 MTUs (RFC 6928)
	usrsctp_sysctl_set_sctp_initial_cwnd(10);

	// Reduce SACK delay from the default 200ms to 20ms
	usrsctp_sysctl_set_sctp_delayed_sack_time_default(20); // ms
}

void SctpTransport::Cleanup() {
	while (usrsctp_finish() != 0)
		std::this_thread::sleep_for(100ms);
}

SctpTransport::SctpTransport(std::shared_ptr<Transport> lower, uint16_t port,
                             message_callback recvCallback, amount_callback bufferedAmountCallback,
                             state_callback stateChangeCallback)
    : Transport(lower, std::move(stateChangeCallback)), mPort(port),
      mSendQueue(0, message_size_func), mBufferedAmountCallback(std::move(bufferedAmountCallback)) {
	onRecv(recvCallback);

	PLOG_DEBUG << "Initializing SCTP transport";

	usrsctp_register_address(this);
	mSock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, &SctpTransport::RecvCallback,
	                       &SctpTransport::SendCallback, 0, this);
	if (!mSock)
		throw std::runtime_error("Could not create SCTP socket, errno=" + std::to_string(errno));

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

	// The sender SHOULD disable the Nagle algorithm (see RFC1122) to minimize the latency.
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13#section-6.6
	int nodelay = 1;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay)))
		throw std::runtime_error("Could not set socket option SCTP_NODELAY, errno=" +
		                         std::to_string(errno));

	struct sctp_paddrparams spp = {};
#if USE_PMTUD
	// Enabled SCTP path MTU discovery
	spp.spp_flags = SPP_PMTUD_ENABLE;
#else
	// Fall back to a safe MTU value.
	spp.spp_flags = SPP_PMTUD_DISABLE;
	spp.spp_pathmtu = 1200; // Max safe value recommended by RFC 8261
	                        // See https://tools.ietf.org/html/rfc8261#section-5
#endif
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &spp, sizeof(spp)))
		throw std::runtime_error("Could not set socket option SCTP_PEER_ADDR_PARAMS, errno=" +
		                         std::to_string(errno));

	// The IETF draft recommends the number of streams negotiated during SCTP association to be
	// 65535. See https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13#section-6.2
	struct sctp_initmsg sinit = {};
	sinit.sinit_num_ostreams = 65535;
	sinit.sinit_max_instreams = 65535;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_INITMSG, &sinit, sizeof(sinit)))
		throw std::runtime_error("Could not set socket option SCTP_INITMSG, errno=" +
		                         std::to_string(errno));

	// Prevent fragmented interleave of messages (i.e. level 0), see RFC 6458 8.1.20.
	// Unless the user has set the fragmentation interleave level to 0, notifications
	// may also be interleaved with partially delivered messages.
	int level = 0;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE, &level, sizeof(level)))
		throw std::runtime_error("Could not disable SCTP fragmented interleave, errno=" +
		                         std::to_string(errno));

	// The default send and receive window size of usrsctp is 256KiB, which is too small for
	// realistic RTTs, therefore we increase it to 1MiB for better performance.
	// See https://bugzilla.mozilla.org/show_bug.cgi?id=1051685
	int bufferSize = 1024 * 1024;
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)))
		throw std::runtime_error("Could not set SCTP recv buffer size, errno=" +
		                         std::to_string(errno));
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)))
		throw std::runtime_error("Could not set SCTP send buffer size, errno=" +
		                         std::to_string(errno));

	registerIncoming();
	connect();
}

SctpTransport::~SctpTransport() {
	stop();

	if (mSock)
		usrsctp_close(mSock);

	usrsctp_deregister_address(this);
}

bool SctpTransport::stop() {
	if (!Transport::stop())
		return false;

	mSendQueue.stop();
	safeFlush();
	shutdown();
	onRecv(nullptr);
	return true;
}

void SctpTransport::connect() {
	if (!mSock)
		return;

	PLOG_DEBUG << "SCTP connect";
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

	// According to the IETF draft, both endpoints must initiate the SCTP association, in a
	// simultaneous-open manner, irrelevent to the SDP setup role.
	// See https://tools.ietf.org/html/draft-ietf-mmusic-sctp-sdp-26#section-9.3
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

	// close() abort the connection when linger is disabled, call it now
	usrsctp_close(mSock);
	mSock = nullptr;

	PLOG_INFO << "SCTP disconnected";
	changeState(State::Disconnected);
	mWrittenCondition.notify_all();
}

bool SctpTransport::send(message_ptr message) {
	std::lock_guard lock(mSendMutex);

	if (!message)
		return mSendQueue.empty();

	PLOG_VERBOSE << "Send size=" << message->size();

	// If nothing is pending, try to send directly
	if (mSendQueue.empty() && trySendMessage(message))
		return true;

	mSendQueue.push(message);
	updateBufferedAmount(message->stream, message_size_func(message));
	return false;
}

void SctpTransport::close(unsigned int stream) {
	send(make_message(0, Message::Reset, uint16_t(stream)));
}

void SctpTransport::flush() {
	std::lock_guard lock(mSendMutex);
	trySendQueue();
}

void SctpTransport::incoming(message_ptr message) {
	// There could be a race condition here where we receive the remote INIT before the local one is
	// sent, which would result in the connection being aborted. Therefore, we need to wait for data
	// to be sent on our side (i.e. the local INIT) before proceeding.
	if (!mWrittenOnce) { // test the atomic boolean is not set first to prevent a lock contention
		std::unique_lock lock(mWriteMutex);
		mWrittenCondition.wait(lock, [&]() { return mWrittenOnce || state() != State::Connected; });
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

bool SctpTransport::trySendQueue() {
	// Requires mSendMutex to be locked
	while (auto next = mSendQueue.peek()) {
		auto message = *next;
		if (!trySendMessage(message))
			return false;
		mSendQueue.pop();
		updateBufferedAmount(message->stream, -message_size_func(message));
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
		sendReset(message->stream);
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
	case Reliability::TYPE_PARTIAL_RELIABLE_REXMIT:
		spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
		spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
		spa.sendv_prinfo.pr_value = uint32_t(std::get<int>(reliability.rexmit));
		break;
	case Reliability::TYPE_PARTIAL_RELIABLE_TIMED:
		spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
		spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
		spa.sendv_prinfo.pr_value = uint32_t(std::get<milliseconds>(reliability.rexmit).count());
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
	if (message->type == Message::Type::Binary || message->type == Message::Type::String)
		mBytesSent += message->size();
	return true;
}

void SctpTransport::updateBufferedAmount(uint16_t streamId, long delta) {
	// Requires mSendMutex to be locked
	auto it = mBufferedAmount.insert(std::make_pair(streamId, 0)).first;
	size_t amount = size_t(std::max(long(it->second) + delta, long(0)));
	if (amount == 0)
		mBufferedAmount.erase(it);
	else
		it->second = amount;

	mSendMutex.unlock();
	try {
		mBufferedAmountCallback(streamId, amount);
	} catch (const std::exception &e) {
		PLOG_WARNING << "SCTP buffered amount callback: " << e.what();
	}
	mSendMutex.lock();
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
		PLOG_VERBOSE << "SCTP stream " << streamId << " already reset";
	} else {
		PLOG_WARNING << "SCTP reset stream " << streamId << " failed, errno=" << errno;
	}
}

bool SctpTransport::safeFlush() {
	try {
		flush();
		return true;

	} catch (const std::exception &e) {
		PLOG_WARNING << "SCTP flush: " << e.what();
		return false;
	}
}

int SctpTransport::handleRecv(struct socket *sock, union sctp_sockstore addr, const byte *data,
                              size_t len, struct sctp_rcvinfo info, int flags) {
	try {
		PLOG_VERBOSE << "Handle recv, len=" << len;
		if (!len)
			return -1;

		// This is valid because SCTP_FRAGMENT_INTERLEAVE is set to level 0
		// so partial messages and notifications may not be interleaved.
		if (flags & MSG_EOR) {
			if (!mPartialRecv.empty()) {
				mPartialRecv.insert(mPartialRecv.end(), data, data + len);
				data = mPartialRecv.data();
				len = mPartialRecv.size();
			}
			// Message/Notification is complete, process it
			if (flags & MSG_NOTIFICATION)
				processNotification(reinterpret_cast<const union sctp_notification *>(data), len);
			else
				processData(data, len, info.rcv_sid, PayloadId(htonl(info.rcv_ppid)));

			mPartialRecv.clear();
		} else {
			// Message/Notification is not complete
			mPartialRecv.insert(mPartialRecv.end(), data, data + len);
		}
	} catch (const std::exception &e) {
		PLOG_ERROR << "SCTP recv: " << e.what();
		return -1;
	}
	return 0; // success
}

int SctpTransport::handleSend(size_t free) {
	PLOG_VERBOSE << "Handle send, free=" << free;
	return safeFlush() ? 0 : -1;
}

int SctpTransport::handleWrite(byte *data, size_t len, uint8_t tos, uint8_t set_df) {
	try {
		PLOG_VERBOSE << "Handle write, len=" << len;

		std::unique_lock lock(mWriteMutex);
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

void SctpTransport::processData(const byte *data, size_t len, uint16_t sid, PayloadId ppid) {
	PLOG_VERBOSE << "Process data, len=" << len;

	// The usage of the PPIDs "WebRTC String Partial" and "WebRTC Binary Partial" is deprecated.
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13#section-6.6
	// We handle them at reception for compatibility reasons but should never send them.
	switch (ppid) {
	case PPID_CONTROL:
		recv(make_message(data, data + len, Message::Control, sid));
		break;

	case PPID_STRING_PARTIAL: // deprecated
		mPartialStringData.insert(mPartialStringData.end(), data, data + len);
		break;

	case PPID_STRING:
		if (mPartialStringData.empty()) {
			mBytesReceived += len;
			recv(make_message(data, data + len, Message::String, sid));
		} else {
			mPartialStringData.insert(mPartialStringData.end(), data, data + len);
			mBytesReceived += mPartialStringData.size();
			recv(make_message(mPartialStringData.begin(), mPartialStringData.end(), Message::String,
			                  sid));
			mPartialStringData.clear();
		}
		break;

	case PPID_STRING_EMPTY:
		// This only accounts for when the partial data is empty
		recv(make_message(mPartialStringData.begin(), mPartialStringData.end(), Message::String,
		                  sid));
		mPartialStringData.clear();
		break;

	case PPID_BINARY_PARTIAL: // deprecated
		mPartialBinaryData.insert(mPartialBinaryData.end(), data, data + len);
		break;

	case PPID_BINARY:
		if (mPartialBinaryData.empty()) {
			mBytesReceived += len;
			recv(make_message(data, data + len, Message::Binary, sid));
		} else {
			mPartialBinaryData.insert(mPartialBinaryData.end(), data, data + len);
			mBytesReceived += mPartialStringData.size();
			recv(make_message(mPartialBinaryData.begin(), mPartialBinaryData.end(), Message::Binary,
			                  sid));
			mPartialBinaryData.clear();
		}
		break;

	case PPID_BINARY_EMPTY:
		// This only accounts for when the partial data is empty
		recv(make_message(mPartialBinaryData.begin(), mPartialBinaryData.end(), Message::Binary,
		                  sid));
		mPartialBinaryData.clear();
		break;

	default:
		// Unknown
		PLOG_WARNING << "Unknown PPID: " << uint32_t(ppid);
		return;
	}
}

void SctpTransport::processNotification(const union sctp_notification *notify, size_t len) {
	if (len != size_t(notify->sn_header.sn_length)) {
		PLOG_WARNING << "Invalid notification length";
		return;
	}

	auto type = notify->sn_header.sn_type;
	PLOG_VERBOSE << "Process notification, type=" << type;

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
		// It not should be necessary since the send callback should have been called already,
		// but to be sure, let's try to send now.
		safeFlush();
		break;
	}

	case SCTP_STREAM_RESET_EVENT: {
		const struct sctp_stream_reset_event &reset_event = notify->sn_strreset_event;
		const int count = (reset_event.strreset_length - sizeof(reset_event)) / sizeof(uint16_t);
		const uint16_t flags = reset_event.strreset_flags;

		if (flags & SCTP_STREAM_RESET_OUTGOING_SSN) {
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event.strreset_stream_list[i];
				close(streamId);
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

std::optional<milliseconds> SctpTransport::rtt() {
	if (!mSock || state() != State::Connected)
		return nullopt;

	struct sctp_status status = {};
	socklen_t len = sizeof(status);
	if (usrsctp_getsockopt(mSock, IPPROTO_SCTP, SCTP_STATUS, &status, &len)) {
		PLOG_WARNING << "Could not read SCTP_STATUS";
		return nullopt;
	}
	return milliseconds(status.sstat_primary.spinfo_srtt);
}

int SctpTransport::RecvCallback(struct socket *sock, union sctp_sockstore addr, void *data,
                                size_t len, struct sctp_rcvinfo recv_info, int flags, void *ptr) {
	int ret = static_cast<SctpTransport *>(ptr)->handleRecv(
	    sock, addr, static_cast<const byte *>(data), len, recv_info, flags);
	free(data);
	return ret;
}

int SctpTransport::SendCallback(struct socket *sock, uint32_t sb_free) {
	struct sctp_paddrinfo paddrinfo = {};
	socklen_t len = sizeof(paddrinfo);
	if (usrsctp_getsockopt(sock, IPPROTO_SCTP, SCTP_GET_PEER_ADDR_INFO, &paddrinfo, &len))
		return -1;

	auto sconn = reinterpret_cast<struct sockaddr_conn *>(&paddrinfo.spinfo_address);
	void *ptr = sconn->sconn_addr;
	return static_cast<SctpTransport *>(ptr)->handleSend(size_t(sb_free));
}

int SctpTransport::WriteCallback(void *ptr, void *data, size_t len, uint8_t tos, uint8_t set_df) {
	return static_cast<SctpTransport *>(ptr)->handleWrite(static_cast<byte *>(data), len, tos,
	                                                      set_df);
}

} // namespace rtc
