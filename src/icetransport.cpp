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

#include "icetransport.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <chrono>
#include <iostream>
#include <random>
#include <sstream>

namespace rtc {

using std::shared_ptr;
using std::weak_ptr;

IceTransport::IceTransport(const Configuration &config, Description::Role role,
                           candidate_callback candidateCallback, state_callback stateChangeCallback,
                           gathering_state_callback gatheringStateChangeCallback)
    : mRole(role), mMid("0"), mState(State::Disconnected), mGatheringState(GatheringState::New),
      mNiceAgent(nullptr, nullptr), mMainLoop(nullptr, nullptr),
      mCandidateCallback(std::move(candidateCallback)),
      mStateChangeCallback(std::move(stateChangeCallback)),
      mGatheringStateChangeCallback(std::move(gatheringStateChangeCallback)) {

	auto logLevelFlags = GLogLevelFlags(G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION);
	g_log_set_handler(nullptr, logLevelFlags, LogCallback, this);
	nice_debug_enable(false);

	mMainLoop = decltype(mMainLoop)(g_main_loop_new(nullptr, FALSE), g_main_loop_unref);
	if (!mMainLoop)
		std::runtime_error("Failed to create the main loop");

	mNiceAgent = decltype(mNiceAgent)(
	    nice_agent_new(g_main_loop_get_context(mMainLoop.get()), NICE_COMPATIBILITY_RFC5245),
	    g_object_unref);

	if (!mNiceAgent)
		throw std::runtime_error("Failed to create the nice agent");

	mMainLoopThread = std::thread(g_main_loop_run, mMainLoop.get());

	g_object_set(G_OBJECT(mNiceAgent.get()), "controlling-mode", TRUE, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "ice-udp", TRUE, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "ice-tcp", FALSE, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "stun-initial-timeout", 200, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "stun-max-retransmissions", 3, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "stun-pacing-timer", 20, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "upnp", FALSE, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "upnp-timeout", 200, nullptr);

	std::vector<IceServer> servers = config.iceServers;
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::shuffle(servers.begin(), servers.end(), std::default_random_engine(seed));

	bool success = false;
	for (auto &server : servers) {
		if (server.hostname.empty())
			continue;
		if (server.type == IceServer::Type::Turn)
			continue;
		if (server.service.empty())
			server.service = "3478"; // STUN UDP port

		struct addrinfo hints = {};
		hints.ai_family = AF_INET; // IPv4
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = AI_ADDRCONFIG;
		struct addrinfo *result = nullptr;
		if (getaddrinfo(server.hostname.c_str(), server.service.c_str(), &hints, &result) != 0)
			continue;

		for (auto p = result; p; p = p->ai_next) {
			if (p->ai_family == AF_INET) {
				char nodebuffer[MAX_NUMERICNODE_LEN];
				char servbuffer[MAX_NUMERICSERV_LEN];
				if (getnameinfo(p->ai_addr, p->ai_addrlen, nodebuffer, MAX_NUMERICNODE_LEN,

				                servbuffer, MAX_NUMERICNODE_LEN,
				                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
					g_object_set(G_OBJECT(mNiceAgent.get()), "stun-server", nodebuffer, nullptr);
					g_object_set(G_OBJECT(mNiceAgent.get()), "stun-server-port",
					             std::stoul(servbuffer), nullptr);
					success = true;
					break;
				}
			}
		}

		freeaddrinfo(result);
		if (success)
			break;
	}

	g_signal_connect(G_OBJECT(mNiceAgent.get()), "component-state-changed",
	                 G_CALLBACK(StateChangeCallback), this);
	g_signal_connect(G_OBJECT(mNiceAgent.get()), "new-candidate-full",
	                 G_CALLBACK(CandidateCallback), this);
	g_signal_connect(G_OBJECT(mNiceAgent.get()), "candidate-gathering-done",
	                 G_CALLBACK(GatheringDoneCallback), this);

	mStreamId = nice_agent_add_stream(mNiceAgent.get(), 1);
	if (!mStreamId)
		throw std::runtime_error("Failed to add a stream");

	nice_agent_set_stream_name(mNiceAgent.get(), mStreamId, "application");
	nice_agent_set_port_range(mNiceAgent.get(), mStreamId, 1, config.portRangeBegin,
	                          config.portRangeEnd);

	nice_agent_attach_recv(mNiceAgent.get(), mStreamId, 1, g_main_loop_get_context(mMainLoop.get()),
	                       RecvCallback, this);

	// Add TURN Servers
	for (auto &server : servers) {
		if (server.hostname.empty())
			continue;
		if (server.type == IceServer::Type::Stun)
			continue;
		if (server.service.empty())
			server.service = "3478"; // TURN UDP port

		struct addrinfo hints = {};
		hints.ai_family = AF_INET; // IPv4
		hints.ai_socktype =
		    server.relayType == IceServer::RelayType::TurnUdp ? SOCK_DGRAM : SOCK_STREAM;
		hints.ai_protocol =
		    server.relayType == IceServer::RelayType::TurnUdp ? IPPROTO_UDP : IPPROTO_TCP;
		hints.ai_flags = AI_ADDRCONFIG;
		struct addrinfo *result = nullptr;
		if (getaddrinfo(server.hostname.c_str(), server.service.c_str(), &hints, &result) != 0)
			continue;

		for (auto p = result; p; p = p->ai_next) {
			if (p->ai_family == AF_INET) {
				char nodebuffer[MAX_NUMERICNODE_LEN];
				char servbuffer[MAX_NUMERICSERV_LEN];
				if (getnameinfo(p->ai_addr, p->ai_addrlen, nodebuffer, MAX_NUMERICNODE_LEN,

				                servbuffer, MAX_NUMERICNODE_LEN,
				                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
					nice_agent_set_relay_info(mNiceAgent.get(), mStreamId, 1, nodebuffer,
					                          std::stoul(servbuffer), server.username.c_str(),
					                          server.password.c_str(),
					                          static_cast<NiceRelayType>(server.relayType));
					break;
				}
			}
		}

		freeaddrinfo(result);
	}
}

IceTransport::~IceTransport() {
	g_main_loop_quit(mMainLoop.get());
	mMainLoopThread.join();
}

Description::Role IceTransport::role() const { return mRole; }

IceTransport::State IceTransport::state() const { return mState; }

Description IceTransport::getLocalDescription(Description::Type type) const {
	// RFC 5245: The agent that generated the offer which started the ICE processing MUST take the
	// controlling role, and the other MUST take the controlled role.
	g_object_set(G_OBJECT(mNiceAgent.get()), "controlling-mode",
	             type == Description::Type::Offer ? TRUE : FALSE, nullptr);

	std::unique_ptr<gchar[], void (*)(void *)> sdp(nice_agent_generate_local_sdp(mNiceAgent.get()),
	                                               g_free);
	return Description(string(sdp.get()), type, mRole);
}

void IceTransport::setRemoteDescription(const Description &description) {
	mRole = description.role() == Description::Role::Active ? Description::Role::Passive
	                                                        : Description::Role::Active;
	mMid = description.mid();

	if (nice_agent_parse_remote_sdp(mNiceAgent.get(), string(description).c_str()) < 0)
		throw std::runtime_error("Failed to parse remote SDP");
}

bool IceTransport::addRemoteCandidate(const Candidate &candidate) {
	// Don't try to pass unresolved candidates to libnice for more safety

	if (!candidate.isResolved())
		return false;

	// Warning: the candidate string must start with "a=candidate:" and it must not end with a
	// newline, else libnice will reject it.
	string sdp(candidate);
	NiceCandidate *cand =
	    nice_agent_parse_remote_candidate_sdp(mNiceAgent.get(), mStreamId, sdp.c_str());
	if (!cand)
		return false;

	GSList *list = g_slist_append(nullptr, cand);
	int ret = nice_agent_set_remote_candidates(mNiceAgent.get(), mStreamId, 1, list);

	g_slist_free_full(list, reinterpret_cast<GDestroyNotify>(nice_candidate_free));
	return ret > 0;
}

void IceTransport::gatherLocalCandidates() {
	// Change state now as candidates calls can be synchronous
	changeGatheringState(GatheringState::InProgress);

	if (!nice_agent_gather_candidates(mNiceAgent.get(), mStreamId)) {
		throw std::runtime_error("Failed to gather local ICE candidates");
	}
}

std::optional<string> IceTransport::getLocalAddress() const {
	NiceCandidate *local = nullptr;
	NiceCandidate *remote = nullptr;
	if (nice_agent_get_selected_pair(mNiceAgent.get(), mStreamId, 1, &local, &remote)) {
		return std::make_optional(AddressToString(local->addr));
	}
	return nullopt;
}
std::optional<string> IceTransport::getRemoteAddress() const {
	NiceCandidate *local = nullptr;
	NiceCandidate *remote = nullptr;
	if (nice_agent_get_selected_pair(mNiceAgent.get(), mStreamId, 1, &local, &remote)) {
		return std::make_optional(AddressToString(remote->addr));
	}
	return nullopt;
}

bool IceTransport::send(message_ptr message) {
	if (!message || !mStreamId)
		return false;

	outgoing(message);
	return true;
}

void IceTransport::incoming(message_ptr message) { recv(message); }

void IceTransport::incoming(const byte *data, int size) {
	incoming(make_message(data, data + size));
}

void IceTransport::outgoing(message_ptr message) {
	nice_agent_send(mNiceAgent.get(), mStreamId, 1, message->size(),
	                reinterpret_cast<const char *>(message->data()));
}

void IceTransport::changeState(State state) {
	if (mState.exchange(state) != state)
		mStateChangeCallback(mState);
}

void IceTransport::changeGatheringState(GatheringState state) {
	mGatheringState = state;
	mGatheringStateChangeCallback(mGatheringState);
}

void IceTransport::processCandidate(const string &candidate) {
	mCandidateCallback(Candidate(candidate, mMid));
}

void IceTransport::processGatheringDone() { changeGatheringState(GatheringState::Complete); }

void IceTransport::processStateChange(uint32_t state) {
	if (state != NICE_COMPONENT_STATE_GATHERING)
		changeState(static_cast<State>(state));
}

string IceTransport::AddressToString(const NiceAddress &addr) {
	char buffer[NICE_ADDRESS_STRING_LEN];
	nice_address_to_string(&addr, buffer);
	unsigned int port = nice_address_get_port(&addr);
	std::ostringstream ss;
	ss << buffer << ":" << port;
	return ss.str();
}

void IceTransport::CandidateCallback(NiceAgent *agent, NiceCandidate *candidate,
                                     gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	gchar *cand = nice_agent_generate_local_candidate_sdp(agent, candidate);
	try {
		iceTransport->processCandidate(cand);
	} catch (const std::exception &e) {
		std::cerr << "ICE candidate: " << e.what() << std::endl;
	}
	g_free(cand);
}

void IceTransport::GatheringDoneCallback(NiceAgent *agent, guint streamId, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processGatheringDone();
	} catch (const std::exception &e) {
		std::cerr << "ICE gathering done: " << e.what() << std::endl;
	}
}

void IceTransport::StateChangeCallback(NiceAgent *agent, guint streamId, guint componentId,
                                       guint state, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processStateChange(state);
	} catch (const std::exception &e) {
		std::cerr << "ICE change state: " << e.what() << std::endl;
	}
}

void IceTransport::RecvCallback(NiceAgent *agent, guint streamId, guint componentId, guint len,
                                gchar *buf, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->incoming(reinterpret_cast<byte *>(buf), len);
	} catch (const std::exception &e) {
		std::cerr << "ICE incoming: " << e.what() << std::endl;
	}
}

void IceTransport::LogCallback(const gchar *logDomain, GLogLevelFlags logLevel,
                               const gchar *message, gpointer userData) {
	std::cout << message << std::endl;
}

} // namespace rtc
