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

#include <exception>
#include <iostream>
#include <vector>

#include <arpa/inet.h>

namespace rtc {

using std::shared_ptr;

SctpTransport::SctpTransport(std::shared_ptr<Transport> lower, ready_callback ready,
                             message_callback recv)
    : Transport(lower), mReadyCallback(std::move(ready)), mLocalPort(5000), mRemotePort(5000) {

	onRecv(recv);

	usrsctp_init(0, &SctpTransport::WriteCallback, nullptr);
	usrsctp_sysctl_set_sctp_ecn_enable(0);
	usrsctp_register_address(this);

	mSock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, &SctpTransport::ReadCallback,
	                       nullptr, 0, this);
	if (!mSock)
		throw std::runtime_error("Could not create usrsctp socket, errno=" + std::to_string(errno));

	struct linger linger_opt = {};
	linger_opt.l_onoff = 1;
	linger_opt.l_linger = 0;
	if (usrsctp_setsockopt(mSock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt)))
		throw std::runtime_error("Could not set socket option SO_LINGER, errno=" +
		                         std::to_string(errno));

	struct sctp_paddrparams peer_param = {};
	peer_param.spp_flags = SPP_PMTUD_DISABLE;
	peer_param.spp_pathmtu = 1200; // TODO: MTU
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &peer_param,
	                       sizeof(peer_param)))
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

	const static uint16_t interested_events[] = {
	    SCTP_ASSOC_CHANGE,          SCTP_PEER_ADDR_CHANGE,       SCTP_REMOTE_ERROR,
	    SCTP_SEND_FAILED,           SCTP_SENDER_DRY_EVENT,       SCTP_SHUTDOWN_EVENT,
	    SCTP_ADAPTATION_INDICATION, SCTP_PARTIAL_DELIVERY_EVENT, SCTP_AUTHENTICATION_EVENT,
	    SCTP_STREAM_RESET_EVENT,    SCTP_ASSOC_RESET_EVENT,      SCTP_STREAM_CHANGE_EVENT,
	    SCTP_SEND_FAILED_EVENT};

	struct sctp_event event = {};
	event.se_assoc_id = SCTP_ALL_ASSOC;
	event.se_on = 1;
	int num_events = sizeof(interested_events) / sizeof(uint16_t);
	for (int i = 0; i < num_events; ++i) {
		event.se_type = interested_events[i];
		if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)))
			throw std::runtime_error("Could not set socket option SCTP_EVENT, errno=" +
			                         std::to_string(errno));
	}

	struct sctp_initmsg init_msg = {};
	init_msg.sinit_num_ostreams = 0xFF;
	init_msg.sinit_max_instreams = 0xFF;
	if (usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_INITMSG, &init_msg, sizeof(init_msg)))
		throw std::runtime_error("Could not set socket option SCTP_INITMSG, errno=" +
		                         std::to_string(errno));

	struct sockaddr_conn sconn = {};
	sconn.sconn_family = AF_CONN;
	sconn.sconn_port = htons(mRemotePort);
	sconn.sconn_addr = (void *)this;
#ifdef HAVE_SCONN_LEN
	sconn.sconn_len = sizeof(sconn);
#endif

	if (usrsctp_bind(mSock, reinterpret_cast<struct sockaddr *>(&sconn), sizeof(sconn)))
		throw std::runtime_error("Could not bind usrsctp socket, errno=" + std::to_string(errno));

	mConnectThread = std::thread(&SctpTransport::runConnect, this);
}

SctpTransport::~SctpTransport() {
	mStopping = true;
	if (mConnectThread.joinable())
		mConnectThread.join();

	if (mSock) {
		usrsctp_shutdown(mSock, SHUT_RDWR);
		usrsctp_close(mSock);
	}

	usrsctp_deregister_address(this);
	usrsctp_finish();
}

bool SctpTransport::isReady() const { return mIsReady; }

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
			const byte dataChannelCloseMessage = byte(0x04);
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

int SctpTransport::handleWrite(void *data, size_t len, uint8_t tos, uint8_t set_df) {
	const byte *b = reinterpret_cast<const byte *>(data);
	outgoing(make_message(b, b + len));
	return 0; // success
}

int SctpTransport::ReadCallback(struct socket *sock, union sctp_sockstore addr, void *data,
                                size_t len, struct sctp_rcvinfo recv_info, int flags,
                                void *user_data) {
	return static_cast<SctpTransport *>(user_data)->process(sock, addr, data, len, recv_info,
	                                                        flags);
}

int SctpTransport::process(struct socket *sock, union sctp_sockstore addr, void *data, size_t len,
                           struct sctp_rcvinfo recv_info, int flags) {
	if (flags & MSG_NOTIFICATION) {
		processNotification((union sctp_notification *)data, len);
	} else {
		processData((const byte *)data, len, recv_info.rcv_sid,
		            PayloadId(ntohl(recv_info.rcv_ppid)));
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
	default:
		type = Message::Control;
		break;
	}
	recv(make_message(data, data + len, type, sid));
}

bool SctpTransport::send(message_ptr message) {
  const Reliability reliability =
      message->reliability ? *message->reliability : Reliability();

  struct sctp_sendv_spa spa = {};

  uint32_t ppid;
  switch (message->type) {
  case Message::String:
    ppid = message->empty() ? PPID_STRING : PPID_STRING_EMPTY;
    break;
  case Message::Binary:
    ppid = message->empty() ? PPID_BINARY : PPID_BINARY_EMPTY;
    break;
  default:
    ppid = PPID_CONTROL;
    break;
	}

	// set sndinfo
	spa.sendv_flags |= SCTP_SEND_SNDINFO_VALID;
	spa.sendv_sndinfo.snd_sid = uint16_t(message->stream);
	spa.sendv_sndinfo.snd_ppid = ppid;
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

	if (!message->empty()) {
		return usrsctp_sendv(mSock, message->data(), message->size(), nullptr, 0, &spa, sizeof(spa),
		                     SCTP_SENDV_SPA, 0) > 0;
	} else {
		const char zero = 0;
		return usrsctp_sendv(mSock, &zero, 1, nullptr, 0, &spa, sizeof(spa), SCTP_SENDV_SPA, 0) > 0;
	}
}

void SctpTransport::reset(unsigned int stream) {
	using reset_streams_t = struct sctp_reset_streams;
	const size_t len = sizeof(reset_streams_t) + sizeof(uint16_t);
	std::byte buffer[len] = {};
	reset_streams_t &reset_streams = *reinterpret_cast<reset_streams_t *>(buffer);
	reset_streams.srs_flags = SCTP_STREAM_RESET_OUTGOING;
	reset_streams.srs_number_streams = 1;
	reset_streams.srs_stream_list[0] = uint16_t(stream);
	usrsctp_setsockopt(mSock, IPPROTO_SCTP, SCTP_RESET_STREAMS, &reset_streams, len);
}

void SctpTransport::incoming(message_ptr message) {
	usrsctp_conninput(this, message->data(), message->size(), 0);
}

void SctpTransport::runConnect() {
	struct sockaddr_conn sconn = {};
	sconn.sconn_family = AF_CONN;
	sconn.sconn_port = htons(mRemotePort);
	sconn.sconn_addr = this;
#ifdef HAVE_SCONN_LEN
	sconn.sconn_len = sizeof(sconn);
#endif

	// Blocks until connection succeeds/fails
	if (usrsctp_connect(mSock, reinterpret_cast<struct sockaddr *>(&sconn), sizeof(sconn)) != 0) {
		mStopping = true;
		return;
	}

        mIsReady = true;
        mReadyCallback();
}
} // namespace rtc
