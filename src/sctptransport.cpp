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
	std::unique_lock<std::mutex> lock(GlobalMutex);
	if (InstancesCount++ == 0) {
		usrsctp_init(0, &SctpTransport::WriteCallback, nullptr);
		usrsctp_sysctl_set_sctp_ecn_enable(0);
	}
}

void SctpTransport::GlobalCleanup() {
	std::unique_lock<std::mutex> lock(GlobalMutex);
	if (--InstancesCount == 0) {
		usrsctp_finish();
	}
}

SctpTransport::SctpTransport(std::shared_ptr<Transport> lower, uint16_t port, message_callback recv,
                             state_callback stateChangeCallback)
    : Transport(lower), mPort(port), mState(State::Disconnected),
      mStateChangeCallback(std::move(stateChangeCallback)) {

	onRecv(recv);

	GlobalInit();
	usrsctp_register_address(this);
	mSock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, &SctpTransport::ReadCallback,
	                       nullptr, 0, this);
	if (!mSock)
		throw std::runtime_error("Could not create usrsctp socket, errno=" + std::to_string(errno));

	struct linger sol = {};
	sol.l_onoff = 1;
	sol.l_linger = 0;
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_LINGER, &sol, sizeof(sol)))
		throw std::runtime_error("Could not set socket option SO_LINGER, errno=" +
		                         std::to_string(errno));

	struct sctp_paddrparams spp = {};
	spp.spp_flags = SPP_PMTUD_ENABLE;
	spp.spp_pathmtu = 1200; // Max safe value recommended by RFC 8261
	                        // See https://tools.ietf.org/html/rfc8261#section-5
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &spp, sizeof(spp)))
		throw std::runtime_error("Could not set socket option SCTP_PEER_ADDR_PARAMS, errno=" +
		                         std::to_string(errno));

	struct sctp_assoc_value av = {};
	av.assoc_id = SCTP_ALL_ASSOC;
	av.assoc_value = 1;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av, sizeof(av)))
		throw std::runtime_error("Could not set socket option SCTP_ENABLE_STREAM_RESET, errno=" +
		                         std::to_string(errno));

	uint32_t nodelay = 1;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay)))
		throw std::runtime_error("Could not set socket option SCTP_NODELAY, errno=" +
		                         std::to_string(errno));

	struct sctp_event se = {};
	se.se_assoc_id = SCTP_ALL_ASSOC;
	se.se_on = 1;
	se.se_type = SCTP_STREAM_RESET_EVENT;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_EVENT, &se, sizeof(se)))
		throw std::runtime_error("Could not set socket option SCTP_EVENT, errno=" +
		                         std::to_string(errno));

	// The IETF draft recommends the number of streams negotiated during SCTP association to be
	// 65535. See https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13#section-6.2
	struct sctp_initmsg sinit = {};
	sinit.sinit_num_ostreams = 65535;
	sinit.sinit_max_instreams = 65535;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_INITMSG, &sinit, sizeof(sinit)))
		throw std::runtime_error("Could not set socket option SCTP_INITMSG, errno=" +
		                         std::to_string(errno));

	struct sockaddr_conn sconn = {};
	sconn.sconn_family = AF_CONN;
	sconn.sconn_port = htons(mPort);
	sconn.sconn_addr = this;
#ifdef HAVE_SCONN_LEN
	sconn.sconn_len = sizeof(sconn);
#endif

	if (usrsctp_bind(mSock, reinterpret_cast<struct sockaddr *>(&sconn), sizeof(sconn)))
		throw std::runtime_error("Could not bind usrsctp socket, errno=" + std::to_string(errno));

	mSendThread = std::thread(&SctpTransport::runConnectAndSendLoop, this);
}

SctpTransport::~SctpTransport() {
	onRecv(nullptr); // unset recv callback
	mStopping = true;
	mConnectCondition.notify_all();
	mSendQueue.stop();

	if (mSock) {
		usrsctp_shutdown(mSock, SHUT_RDWR);
		usrsctp_close(mSock);
	}

	mSendThread.join();

	usrsctp_deregister_address(this);
	GlobalCleanup();
}

SctpTransport::State SctpTransport::state() const { return mState; }

bool SctpTransport::send(message_ptr message) {
	if (!message || mStopping)
		return false;

	mSendQueue.push(message);
	return true;
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

	// There could be a race condition here where we receive the remote INIT before the thread in
	// usrsctp_connect sends the local one, which would result in the connection being aborted.
	// Therefore, we need to wait for data to be sent on our side (i.e. the local INIT) before
	// proceeding.
	if (!mConnectDataSent) {
		std::unique_lock<std::mutex> lock(mConnectMutex);
		mConnectCondition.wait(lock, [this] { return mConnectDataSent || mStopping; });
	}

	if (!mStopping)
		usrsctp_conninput(this, message->data(), message->size(), 0);
}

void SctpTransport::changeState(State state) {
	mState = state;
	mStateChangeCallback(state);
}

void SctpTransport::runConnectAndSendLoop() {
	try {
		changeState(State::Connecting);

		struct sockaddr_conn sconn = {};
		sconn.sconn_family = AF_CONN;
		sconn.sconn_port = htons(mPort);
		sconn.sconn_addr = this;
#ifdef HAVE_SCONN_LEN
		sconn.sconn_len = sizeof(sconn);
#endif

		// According to the IETF draft, both endpoints must initiate the SCTP association, in a
		// simultaneous-open manner, irrelevent to the SDP setup role.
		// See https://tools.ietf.org/html/draft-ietf-mmusic-sctp-sdp-26#section-9.3
		if (usrsctp_connect(mSock, reinterpret_cast<struct sockaddr *>(&sconn), sizeof(sconn)) != 0)
			throw std::runtime_error("Connection failed, errno=" + std::to_string(errno));

		if (!mStopping)
			changeState(State::Connected);

	} catch (const std::exception &e) {
		std::cerr << "SCTP connect: " << e.what() << std::endl;
		changeState(State::Failed);
		mStopping = true;
		mConnectCondition.notify_all();
		return;
	}

	try {
		while (auto message = mSendQueue.pop()) {
			if (!doSend(*message))
				throw std::runtime_error("Sending failed, errno=" + std::to_string(errno));
		}
	} catch (const std::exception &e) {
		std::cerr << "SCTP send: " << e.what() << std::endl;
	}

	changeState(State::Disconnected);
	mStopping = true;
	mConnectCondition.notify_all();
}

bool SctpTransport::doSend(message_ptr message) {
	if (!message)
		return false;

	const Reliability reliability = message->reliability ? *message->reliability : Reliability();

	struct sctp_sendv_spa spa = {};

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

	// set sndinfo
	spa.sendv_flags |= SCTP_SEND_SNDINFO_VALID;
	spa.sendv_sndinfo.snd_sid = uint16_t(message->stream);
	spa.sendv_sndinfo.snd_ppid = htonl(ppid);
	spa.sendv_sndinfo.snd_flags |= SCTP_EOR;

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
	return ret > 0;
}

int SctpTransport::handleWrite(void *data, size_t len, uint8_t tos, uint8_t set_df) {
	byte *b = reinterpret_cast<byte *>(data);
	outgoing(make_message(b, b + len));

	if (!mConnectDataSent) {
		std::unique_lock<std::mutex> lock(mConnectMutex);
		mConnectDataSent = true;
		mConnectCondition.notify_all();
	}
	return 0; // success
}

int SctpTransport::process(struct socket *sock, union sctp_sockstore addr, void *data, size_t len,
                           struct sctp_rcvinfo info, int flags) {
	if (!data)
		recv(nullptr);
	if (flags & MSG_NOTIFICATION) {
		processNotification((union sctp_notification *)data, len);
	} else {
		processData((const byte *)data, len, info.rcv_sid, PayloadId(htonl(info.rcv_ppid)));
	}
	free(data);
	return 0;
}

void SctpTransport::processData(const byte *data, size_t len, uint16_t sid, PayloadId ppid) {
	Message::Type type;
	switch (ppid) {
	case PPID_STRING:
		type = Message::String;
		break;
	case PPID_STRING_EMPTY:
		type = Message::String;
		len = 0;
		break;
	case PPID_BINARY:
		type = Message::Binary;
		break;
	case PPID_BINARY_EMPTY:
		type = Message::Binary;
		len = 0;
		break;
	case PPID_CONTROL:
		type = Message::Control;
		break;
	default:
		// Unknown
		std::cerr << "Unknown PPID: " << uint32_t(ppid) << std::endl;
		return;
	}
	recv(make_message(data, data + len, type, sid));
}

void SctpTransport::processNotification(const union sctp_notification *notify, size_t len) {
	if (len != size_t(notify->sn_header.sn_length))
		return;

	switch (notify->sn_header.sn_type) {
	case SCTP_STREAM_RESET_EVENT: {
		const struct sctp_stream_reset_event *reset_event = &notify->sn_strreset_event;
		const int count = (reset_event->strreset_length - sizeof(*reset_event)) / sizeof(uint16_t);

		if (reset_event->strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event->strreset_stream_list[i];
				reset(streamId);
			}
		}

		if (reset_event->strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN) {
			const byte dataChannelCloseMessage{0x04};
			for (int i = 0; i < count; ++i) {
				uint16_t streamId = reset_event->strreset_stream_list[i];
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
int SctpTransport::WriteCallback(void *sctp_ptr, void *data, size_t len, uint8_t tos,
                                 uint8_t set_df) {
	return static_cast<SctpTransport *>(sctp_ptr)->handleWrite(data, len, tos, set_df);
}

int SctpTransport::ReadCallback(struct socket *sock, union sctp_sockstore addr, void *data,
                                size_t len, struct sctp_rcvinfo recv_info, int flags,
                                void *user_data) {
	return static_cast<SctpTransport *>(user_data)->process(sock, addr, data, len, recv_info,
	                                                        flags);
}

} // namespace rtc
