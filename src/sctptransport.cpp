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
#include <vector>

#include <arpa/inet.h>

using std::shared_ptr;

namespace rtc {

std::mutex SctpTransport::GlobalMutex;
int SctpTransport::InstancesCount = 0;

void SctpTransport::GlobalInit() {
	std::lock_guard lock(GlobalMutex);
	if (InstancesCount++ == 0) {
		usrsctp_init(0, &SctpTransport::WriteCallback, nullptr);
		usrsctp_sysctl_set_sctp_ecn_enable(0);
	}
}

void SctpTransport::GlobalCleanup() {
	std::lock_guard lock(GlobalMutex);
	if (--InstancesCount == 0) {
		usrsctp_finish();
	}
}

SctpTransport::SctpTransport(std::shared_ptr<Transport> lower, uint16_t port,
                             message_callback recvCallback, amount_callback bufferedAmountCallback,
                             state_callback stateChangeCallback)
    : Transport(lower), mPort(port), mSendQueue(0, message_size_func),
      mBufferedAmountCallback(std::move(bufferedAmountCallback)),
      mStateChangeCallback(std::move(stateChangeCallback)), mState(State::Disconnected) {
	onRecv(recvCallback);

	GlobalInit();

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
#ifdef __linux__
	// Linux UDP does path MTU discovery by default (setting DF and returning EMSGSIZE).
	// It should be safe to enable discovery for SCTP.
	spp.spp_flags = SPP_PMTUD_ENABLE;
#else
	// Otherwise, fall back to a safe MTU value.
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

	// The default send and receive window size of usrsctp is 256KiB, which is too small for
	// realistic RTTs, therefore we increase it to 1MiB for better performance.
	// See https://bugzilla.mozilla.org/show_bug.cgi?id=1051685
	int bufSize = 1024 * 1024;
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize)))
		throw std::runtime_error("Could not set SCTP recv buffer size, errno=" +
		                         std::to_string(errno));
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize)))
		throw std::runtime_error("Could not set SCTP send buffer size, errno=" +
		                         std::to_string(errno));

	connect();
}

SctpTransport::~SctpTransport() {
	stop();

	usrsctp_shutdown(mSock, SHUT_RDWR);
	usrsctp_close(mSock);
	usrsctp_deregister_address(this);

	GlobalCleanup();
}

SctpTransport::State SctpTransport::state() const { return mState; }

void SctpTransport::stop() {
	Transport::stop();
	onRecv(nullptr);

	mSendQueue.stop();

	// Unblock incoming
	std::unique_lock<std::mutex> lock(mConnectMutex);
	mConnectDataSent = true;
	mConnectCondition.notify_all();
}

void SctpTransport::connect() {
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

bool SctpTransport::send(message_ptr message) {
	std::lock_guard lock(mSendMutex);

	if (!message)
		return mSendQueue.empty();

	// If nothing is pending, try to send directly
	if (mSendQueue.empty() && trySendMessage(message))
		return true;

	mSendQueue.push(message);
	updateBufferedAmount(message->stream, message_size_func(message));
	return false;
}

void SctpTransport::reset(unsigned int stream) {
	using srs_t = struct sctp_reset_streams;
	const size_t len = sizeof(srs_t) + sizeof(uint16_t);
	byte buffer[len] = {};
	srs_t &srs = *reinterpret_cast<srs_t *>(buffer);
	srs.srs_flags = SCTP_STREAM_RESET_OUTGOING;
	srs.srs_number_streams = 1;
	srs.srs_stream_list[0] = uint16_t(stream);
	usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_RESET_STREAMS, &srs, len);
}

void SctpTransport::incoming(message_ptr message) {
	if (!message) {
		changeState(State::Disconnected);
		recv(nullptr);
		return;
	}

	// There could be a race condition here where we receive the remote INIT before the local one is
	// sent, which would result in the connection being aborted. Therefore, we need to wait for data
	// to be sent on our side (i.e. the local INIT) before proceeding.
	{
		std::unique_lock lock(mConnectMutex);
		mConnectCondition.wait(lock, [this]() -> bool { return mConnectDataSent; });
	}

	usrsctp_conninput(this, message->data(), message->size(), 0);
}

void SctpTransport::changeState(State state) {
	if (mState.exchange(state) != state)
		mStateChangeCallback(state);
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
	//
	// TODO: Implement SCTP ndata specification draft when supported everywhere
	// See https://tools.ietf.org/html/draft-ietf-tsvwg-sctp-ndata-08

	const Reliability reliability = message->reliability ? *message->reliability : Reliability();

	uint32_t ppid;
	switch (message->type) {
	case Message::String:
		ppid = !message->empty() ? PPID_STRING : PPID_STRING_EMPTY;
		break;
	case Message::Binary:
		ppid = !message->empty() ? PPID_BINARY : PPID_BINARY_EMPTY;
		break;
	default:
		ppid = PPID_CONTROL;
		break;
	}

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

	using std::chrono::milliseconds;
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

	if (ret >= 0)
		return true;
	else if (errno == EWOULDBLOCK && errno == EAGAIN)
		return false;
	else
		throw std::runtime_error("Sending failed, errno=" + std::to_string(errno));
}

void SctpTransport::updateBufferedAmount(uint16_t streamId, long delta) {
	// Requires mSendMutex to be locked
	auto it = mBufferedAmount.insert(std::make_pair(streamId, 0)).first;
	size_t amount = it->second;
	amount = size_t(std::max(long(amount) + delta, long(0)));
	if (amount == 0)
		mBufferedAmount.erase(it);
	mBufferedAmountCallback(streamId, amount);
}

int SctpTransport::handleRecv(struct socket *sock, union sctp_sockstore addr, const byte *data,
                              size_t len, struct sctp_rcvinfo info, int flags) {
	try {
		if (!data) {
			recv(nullptr);
			return 0;
		}
		if (flags & MSG_EOR) {
			if (!mPartialRecv.empty()) {
				mPartialRecv.insert(mPartialRecv.end(), data, data + len);
				data = mPartialRecv.data();
				len = mPartialRecv.size();
			}
			// Message is complete, process it
			if (flags & MSG_NOTIFICATION)
				processNotification(reinterpret_cast<const union sctp_notification *>(data), len);
			else
				processData(data, len, info.rcv_sid, PayloadId(htonl(info.rcv_ppid)));

			mPartialRecv.clear();
		} else {
			// Message is not complete
			mPartialRecv.insert(mPartialRecv.end(), data, data + len);
		}
	} catch (const std::exception &e) {
		std::cerr << "SCTP recv: " << e.what() << std::endl;
		return -1;
	}
	return 0; // success
}

int SctpTransport::handleSend(size_t free) {
	try {
		std::lock_guard lock(mSendMutex);
		trySendQueue();
	} catch (const std::exception &e) {
		std::cerr << "SCTP send: " << e.what() << std::endl;
		return -1;
	}
	return 0; // success
}

int SctpTransport::handleWrite(byte *data, size_t len, uint8_t tos, uint8_t set_df) {
	try {
		outgoing(make_message(data, data + len));

		std::unique_lock lock(mConnectMutex);
		mConnectDataSent = true;
		mConnectCondition.notify_all();
	} catch (const std::exception &e) {
		std::cerr << "SCTP write: " << e.what() << std::endl;
		return -1;
	}
	return 0; // success
}

void SctpTransport::processData(const byte *data, size_t len, uint16_t sid, PayloadId ppid) {
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
			recv(make_message(data, data + len, Message::String, sid));
		} else {
			mPartialStringData.insert(mPartialStringData.end(), data, data + len);
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
			recv(make_message(data, data + len, Message::Binary, sid));
		} else {
			mPartialBinaryData.insert(mPartialBinaryData.end(), data, data + len);
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
		std::cerr << "Unknown PPID: " << uint32_t(ppid) << std::endl;
		return;
	}
}

void SctpTransport::processNotification(const union sctp_notification *notify, size_t len) {
	if (len != size_t(notify->sn_header.sn_length))
		return;

	switch (notify->sn_header.sn_type) {
	case SCTP_ASSOC_CHANGE: {
		const struct sctp_assoc_change &assoc_change = notify->sn_assoc_change;
		if (assoc_change.sac_state == SCTP_COMM_UP) {
			changeState(State::Connected);
		} else {
			if (mState == State::Connecting) {
				std::cerr << "SCTP connection failed" << std::endl;
				changeState(State::Failed);
			} else {
				changeState(State::Disconnected);
			}
		}
	}
	case SCTP_SENDER_DRY_EVENT: {
		// It not should be necessary since the send callback should have been called already,
		// but to be sure, let's try to send now.
		std::lock_guard lock(mSendMutex);
		trySendQueue();
	}
	case SCTP_STREAM_RESET_EVENT: {
		const struct sctp_stream_reset_event &reset_event = notify->sn_strreset_event;
		const int count = (reset_event.strreset_length - sizeof(reset_event)) / sizeof(uint16_t);

		if (reset_event.strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event.strreset_stream_list[i];
				reset(streamId);
			}
		}

		if (reset_event.strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN) {
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
