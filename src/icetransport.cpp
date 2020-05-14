/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
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
#include "configuration.hpp"
#include "transport.hpp"

#include <iostream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <sys/types.h>

using namespace std::chrono_literals;

using std::shared_ptr;
using std::weak_ptr;

#if USE_JUICE

namespace rtc {

IceTransport::IceTransport(const Configuration &config, Description::Role role,
                           candidate_callback candidateCallback, state_callback stateChangeCallback,
                           gathering_state_callback gatheringStateChangeCallback)
    : mRole(role), mMid("0"), mState(State::Disconnected), mGatheringState(GatheringState::New),
      mCandidateCallback(std::move(candidateCallback)),
      mStateChangeCallback(std::move(stateChangeCallback)),
      mGatheringStateChangeCallback(std::move(gatheringStateChangeCallback)),
      mAgent(nullptr, nullptr) {

	PLOG_DEBUG << "Initializing ICE transport (libjuice)";
	if (config.enableIceTcp) {
		PLOG_WARNING << "ICE-TCP is not supported with libjuice";
	}
	juice_set_log_handler(IceTransport::LogCallback);
	juice_set_log_level(JUICE_LOG_LEVEL_VERBOSE);

	juice_config_t jconfig = {};
	jconfig.cb_state_changed = IceTransport::StateChangeCallback;
	jconfig.cb_candidate = IceTransport::CandidateCallback;
	jconfig.cb_gathering_done = IceTransport::GatheringDoneCallback;
	jconfig.cb_recv = IceTransport::RecvCallback;
	jconfig.user_ptr = this;

	// Randomize servers order
	std::vector<IceServer> servers = config.iceServers;
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::shuffle(servers.begin(), servers.end(), std::default_random_engine(seed));

	// Pick a STUN server (TURN support is not implemented in libjuice yet)
	for (auto &server : servers) {
		if (!server.hostname.empty() && server.type == IceServer::Type::Stun) {
			if (server.service.empty())
				server.service = "3478"; // STUN UDP port
			PLOG_DEBUG << "Using STUN server \"" << server.hostname << ":" << server.service
			           << "\"";
			mStunHostname = server.hostname;
			mStunService = server.service;
			jconfig.stun_server_host = mStunHostname.c_str();
			jconfig.stun_server_port = std::stoul(mStunService);
		}
	}

	// Port range
	if (config.portRangeBegin > 1024 ||
	    (config.portRangeEnd != 0 && config.portRangeEnd != 65535)) {
		jconfig.local_port_range_begin = config.portRangeBegin;
		jconfig.local_port_range_end = config.portRangeEnd;
	}

	// Create agent
	mAgent = decltype(mAgent)(juice_create(&jconfig), juice_destroy);
	if (!mAgent)
		throw std::runtime_error("Failed to create the ICE agent");
}

IceTransport::~IceTransport() { stop(); }

bool IceTransport::stop() {
	onRecv(nullptr);
	return Transport::stop();
}

Description::Role IceTransport::role() const { return mRole; }

IceTransport::State IceTransport::state() const { return mState; }

Description IceTransport::getLocalDescription(Description::Type type) const {
	char sdp[JUICE_MAX_SDP_STRING_LEN];
	if (juice_get_local_description(mAgent.get(), sdp, JUICE_MAX_SDP_STRING_LEN) < 0)
		throw std::runtime_error("Failed to generate local SDP");

	return Description(string(sdp), type, mRole);
}

void IceTransport::setRemoteDescription(const Description &description) {
	mRole = description.role() == Description::Role::Active ? Description::Role::Passive
	                                                        : Description::Role::Active;
	mMid = description.mid();
	if (juice_set_remote_description(mAgent.get(), string(description).c_str()) < 0)
		throw std::runtime_error("Failed to parse remote SDP");
}

bool IceTransport::addRemoteCandidate(const Candidate &candidate) {
	// Don't try to pass unresolved candidates for more safety
	if (!candidate.isResolved())
		return false;

	return juice_add_remote_candidate(mAgent.get(), string(candidate).c_str()) >= 0;
}

void IceTransport::gatherLocalCandidates() {
	// Change state now as candidates calls can be synchronous
	changeGatheringState(GatheringState::InProgress);

	if (juice_gather_candidates(mAgent.get()) < 0) {
		throw std::runtime_error("Failed to gather local ICE candidates");
	}
}

std::optional<string> IceTransport::getLocalAddress() const {
	char str[JUICE_MAX_ADDRESS_STRING_LEN];
	if (juice_get_selected_addresses(mAgent.get(), str, JUICE_MAX_ADDRESS_STRING_LEN, NULL, 0) ==
	    0) {
		return std::make_optional(string(str));
	}
	return nullopt;
}
std::optional<string> IceTransport::getRemoteAddress() const {
	char str[JUICE_MAX_ADDRESS_STRING_LEN];
	if (juice_get_selected_addresses(mAgent.get(), NULL, 0, str, JUICE_MAX_ADDRESS_STRING_LEN) ==
	    0) {
		return std::make_optional(string(str));
	}
	return nullopt;
}

bool IceTransport::send(message_ptr message) {
	if (!message || (mState != State::Connected && mState != State::Completed))
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();
	return outgoing(message);
}

void IceTransport::incoming(message_ptr message) {
	PLOG_VERBOSE << "Incoming size=" << message->size();
	if (recvCallbackValid())
		recv(message);
	else
		PLOG_WARNING << "Received data but mRecvCallback is not valid";
}

void IceTransport::incoming(const byte *data, int size) {
	incoming(make_message(data, data + size));
}

bool IceTransport::outgoing(message_ptr message) {
	return juice_send(mAgent.get(), reinterpret_cast<const char *>(message->data()),
	                  message->size()) >= 0;
}

void IceTransport::changeState(State state) {
	if (mState.exchange(state) != state)
		mStateChangeCallback(mState);
}

void IceTransport::changeGatheringState(GatheringState state) {
	if (mGatheringState.exchange(state) != state)
		mGatheringStateChangeCallback(mGatheringState);
}

void IceTransport::processStateChange(unsigned int state) {
	changeState(static_cast<State>(state));
}

void IceTransport::processCandidate(const string &candidate) {
	mCandidateCallback(Candidate(candidate, mMid));
}

void IceTransport::processGatheringDone() { changeGatheringState(GatheringState::Complete); }

void IceTransport::StateChangeCallback(juice_agent_t *agent, juice_state_t state, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->processStateChange(static_cast<unsigned int>(state));
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::CandidateCallback(juice_agent_t *agent, const char *sdp, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->processCandidate(sdp);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::GatheringDoneCallback(juice_agent_t *agent, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->processGatheringDone();
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::RecvCallback(juice_agent_t *agent, const char *data, size_t size,
                                void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->incoming(reinterpret_cast<const byte *>(data), size);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::LogCallback(juice_log_level_t level, const char *message) {
	plog::Severity severity;
	switch (level) {
	case JUICE_LOG_LEVEL_FATAL:
		severity = plog::fatal;
		break;
	case JUICE_LOG_LEVEL_ERROR:
		severity = plog::error;
		break;
	case JUICE_LOG_LEVEL_WARN:
		severity = plog::warning;
		break;
	case JUICE_LOG_LEVEL_INFO:
		severity = plog::info;
		break;
	default:
		severity = plog::verbose; // libjuice debug as verbose
		break;
	}
	PLOG(severity) << "juice: " << message;
}

} // namespace rtc

#else // USE_JUICE == 0

namespace rtc {

IceTransport::IceTransport(const Configuration &config, Description::Role role,
                           candidate_callback candidateCallback, state_callback stateChangeCallback,
                           gathering_state_callback gatheringStateChangeCallback)
    : mRole(role), mMid("0"), mState(State::Disconnected), mGatheringState(GatheringState::New),
      mCandidateCallback(std::move(candidateCallback)),
      mStateChangeCallback(std::move(stateChangeCallback)),
      mGatheringStateChangeCallback(std::move(gatheringStateChangeCallback)),
      mNiceAgent(nullptr, nullptr), mMainLoop(nullptr, nullptr) {

	PLOG_DEBUG << "Initializing ICE transport (libnice)";

	g_log_set_handler("libnice", G_LOG_LEVEL_MASK, LogCallback, this);

	IF_PLOG(plog::verbose) {
		nice_debug_enable(false); // do not output STUN debug messages
	}

	mMainLoop = decltype(mMainLoop)(g_main_loop_new(nullptr, FALSE), g_main_loop_unref);
	if (!mMainLoop)
		std::runtime_error("Failed to create the main loop");

	// RFC 5245 was obsoleted by RFC 8445 but this should be OK.
	mNiceAgent = decltype(mNiceAgent)(
	    nice_agent_new(g_main_loop_get_context(mMainLoop.get()), NICE_COMPATIBILITY_RFC5245),
	    g_object_unref);

	if (!mNiceAgent)
		throw std::runtime_error("Failed to create the nice agent");

	mMainLoopThread = std::thread(g_main_loop_run, mMainLoop.get());

	mStreamId = nice_agent_add_stream(mNiceAgent.get(), 1);
	if (!mStreamId)
		throw std::runtime_error("Failed to add a stream");

	g_object_set(G_OBJECT(mNiceAgent.get()), "controlling-mode", TRUE, nullptr); // decided later
	g_object_set(G_OBJECT(mNiceAgent.get()), "ice-udp", TRUE, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "ice-tcp", config.enableIceTcp ? TRUE : FALSE,
	             nullptr);

	// RFC 8445: Agents MUST NOT use an RTO value smaller than 500 ms.
	g_object_set(G_OBJECT(mNiceAgent.get()), "stun-initial-timeout", 500, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "stun-max-retransmissions", 3, nullptr);

	// RFC 8445: ICE agents SHOULD use a default Ta value, 50 ms, but MAY use another value based on
	// the characteristics of the associated data.
	g_object_set(G_OBJECT(mNiceAgent.get()), "stun-pacing-timer", 25, nullptr);

	g_object_set(G_OBJECT(mNiceAgent.get()), "upnp", FALSE, nullptr);
	g_object_set(G_OBJECT(mNiceAgent.get()), "upnp-timeout", 200, nullptr);

	// Proxy
	if (config.proxyServer.has_value()) {
		ProxyServer proxyServer = config.proxyServer.value();
		g_object_set(G_OBJECT(mNiceAgent.get()), "proxy-type", proxyServer.type, nullptr);
		g_object_set(G_OBJECT(mNiceAgent.get()), "proxy-ip", proxyServer.ip.c_str(), nullptr);
		g_object_set(G_OBJECT(mNiceAgent.get()), "proxy-port", proxyServer.port, nullptr);
		g_object_set(G_OBJECT(mNiceAgent.get()), "proxy-username", proxyServer.username.c_str(),
		             nullptr);
		g_object_set(G_OBJECT(mNiceAgent.get()), "proxy-password", proxyServer.password.c_str(),
		             nullptr);
	}

	// Randomize order
	std::vector<IceServer> servers = config.iceServers;
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::shuffle(servers.begin(), servers.end(), std::default_random_engine(seed));

	// Add one STUN server
	bool success = false;
	for (auto &server : servers) {
		if (server.hostname.empty())
			continue;
		if (server.type != IceServer::Type::Stun)
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
					PLOG_DEBUG << "Using STUN server \"" << server.hostname << ":" << server.service
					           << "\"";
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

	// Add TURN servers
	for (auto &server : servers) {
		if (server.hostname.empty())
			continue;
		if (server.type != IceServer::Type::Turn)
			continue;
		if (server.service.empty())
			server.service = server.relayType == IceServer::RelayType::TurnTls ? "5349" : "3478";

		struct addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype =
		    server.relayType == IceServer::RelayType::TurnUdp ? SOCK_DGRAM : SOCK_STREAM;
		hints.ai_protocol =
		    server.relayType == IceServer::RelayType::TurnUdp ? IPPROTO_UDP : IPPROTO_TCP;
		hints.ai_flags = AI_ADDRCONFIG;
		struct addrinfo *result = nullptr;
		if (getaddrinfo(server.hostname.c_str(), server.service.c_str(), &hints, &result) != 0)
			continue;

		for (auto p = result; p; p = p->ai_next) {
			if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
				char nodebuffer[MAX_NUMERICNODE_LEN];
				char servbuffer[MAX_NUMERICSERV_LEN];
				if (getnameinfo(p->ai_addr, p->ai_addrlen, nodebuffer, MAX_NUMERICNODE_LEN,
				                servbuffer, MAX_NUMERICNODE_LEN,
				                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {

					NiceRelayType niceRelayType;
					switch (server.relayType) {
					case IceServer::RelayType::TurnTcp:
						niceRelayType = NICE_RELAY_TYPE_TURN_TCP;
						break;
					case IceServer::RelayType::TurnTls:
						niceRelayType = NICE_RELAY_TYPE_TURN_TLS;
						break;
					default:
						niceRelayType = NICE_RELAY_TYPE_TURN_UDP;
						break;
					}
					nice_agent_set_relay_info(mNiceAgent.get(), mStreamId, 1, nodebuffer,
					                          std::stoul(servbuffer), server.username.c_str(),
					                          server.password.c_str(), niceRelayType);
				}
			}
		}

		freeaddrinfo(result);
	}

	g_signal_connect(G_OBJECT(mNiceAgent.get()), "component-state-changed",
	                 G_CALLBACK(StateChangeCallback), this);
	g_signal_connect(G_OBJECT(mNiceAgent.get()), "new-candidate-full",
	                 G_CALLBACK(CandidateCallback), this);
	g_signal_connect(G_OBJECT(mNiceAgent.get()), "candidate-gathering-done",
	                 G_CALLBACK(GatheringDoneCallback), this);

	nice_agent_set_stream_name(mNiceAgent.get(), mStreamId, "application");
	nice_agent_set_port_range(mNiceAgent.get(), mStreamId, 1, config.portRangeBegin,
	                          config.portRangeEnd);

	nice_agent_attach_recv(mNiceAgent.get(), mStreamId, 1, g_main_loop_get_context(mMainLoop.get()),
	                       RecvCallback, this);
}

IceTransport::~IceTransport() { stop(); }

bool IceTransport::stop() {
	if (mTimeoutId) {
		g_source_remove(mTimeoutId);
		mTimeoutId = 0;
	}

	if (!Transport::stop())
		return false;

	PLOG_DEBUG << "Stopping ICE thread";
	g_main_loop_quit(mMainLoop.get());
	mMainLoopThread.join();
	return true;
}

Description::Role IceTransport::role() const { return mRole; }

IceTransport::State IceTransport::state() const { return mState; }

Description IceTransport::getLocalDescription(Description::Type type) const {
	// RFC 8445: The initiating agent that started the ICE processing MUST take the controlling
	// role, and the other MUST take the controlled role.
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
	mTrickleTimeout = description.trickleEnabled() ? 30s : 0s;

	// Warning: libnice expects "\n" as end of line
	if (nice_agent_parse_remote_sdp(mNiceAgent.get(), description.generateSdp("\n").c_str()) < 0)
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
	if (!message || (mState != State::Connected && mState != State::Completed))
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();
	return outgoing(message);
}

void IceTransport::incoming(message_ptr message) {
	PLOG_VERBOSE << "Incoming size=" << message->size();
	if (recvCallbackValid())
		recv(message);
	else
		PLOG_WARNING << "Received data but mRecvCallback is not valid";
}

void IceTransport::incoming(const byte *data, int size) {
	incoming(make_message(data, data + size));
}

bool IceTransport::outgoing(message_ptr message) {
	return nice_agent_send(mNiceAgent.get(), mStreamId, 1, message->size(),
	                       reinterpret_cast<const char *>(message->data())) >= 0;
}

void IceTransport::changeState(State state) {
	if (mState.exchange(state) != state)
		mStateChangeCallback(mState);
}

void IceTransport::changeGatheringState(GatheringState state) {
	if (mGatheringState.exchange(state) != state)
		mGatheringStateChangeCallback(mGatheringState);
}

void IceTransport::processTimeout() {
	PLOG_WARNING << "ICE timeout";
	mTimeoutId = 0;
	changeState(State::Failed);
}

void IceTransport::processCandidate(const string &candidate) {
	mCandidateCallback(Candidate(candidate, mMid));
}

void IceTransport::processGatheringDone() { changeGatheringState(GatheringState::Complete); }

void IceTransport::processStateChange(unsigned int state) {
	if (state == NICE_COMPONENT_STATE_FAILED && mTrickleTimeout.count() > 0) {
		if (mTimeoutId)
			g_source_remove(mTimeoutId);
		mTimeoutId = g_timeout_add(mTrickleTimeout.count() /* ms */, TimeoutCallback, this);
		return;
	}

	if (state == NICE_COMPONENT_STATE_CONNECTED && mTimeoutId) {
		g_source_remove(mTimeoutId);
		mTimeoutId = 0;
	}

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
		PLOG_WARNING << e.what();
	}
	g_free(cand);
}

void IceTransport::GatheringDoneCallback(NiceAgent *agent, guint streamId, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processGatheringDone();
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::StateChangeCallback(NiceAgent *agent, guint streamId, guint componentId,
                                       guint state, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processStateChange(state);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::RecvCallback(NiceAgent *agent, guint streamId, guint componentId, guint len,
                                gchar *buf, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->incoming(reinterpret_cast<byte *>(buf), len);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

gboolean IceTransport::TimeoutCallback(gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processTimeout();
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
	return FALSE;
}

void IceTransport::LogCallback(const gchar *logDomain, GLogLevelFlags logLevel,
                               const gchar *message, gpointer userData) {
	plog::Severity severity;
	unsigned int flags = logLevel & G_LOG_LEVEL_MASK;
	if (flags & G_LOG_LEVEL_ERROR)
		severity = plog::fatal;
	else if (flags & G_LOG_LEVEL_CRITICAL)
		severity = plog::error;
	else if (flags & G_LOG_LEVEL_WARNING)
		severity = plog::warning;
	else if (flags & G_LOG_LEVEL_MESSAGE)
		severity = plog::info;
	else if (flags & G_LOG_LEVEL_INFO)
		severity = plog::info;
	else
		severity = plog::verbose; // libnice debug as verbose

	PLOG(severity) << "nice: " << message;
}

bool IceTransport::getSelectedCandidatePair(CandidateInfo *localInfo, CandidateInfo *remoteInfo) {
	NiceCandidate *local, *remote;
	gboolean result = nice_agent_get_selected_pair(mNiceAgent.get(), mStreamId, 1, &local, &remote);

	if (!result)
		return false;

	char ipaddr[INET6_ADDRSTRLEN];
	nice_address_to_string(&local->addr, ipaddr);
	localInfo->address = std::string(ipaddr);
	localInfo->port = nice_address_get_port(&local->addr);
	localInfo->type = IceTransport::NiceTypeToCandidateType(local->type);
	localInfo->transportType =
	    IceTransport::NiceTransportTypeToCandidateTransportType(local->transport);

	nice_address_to_string(&remote->addr, ipaddr);
	remoteInfo->address = std::string(ipaddr);
	remoteInfo->port = nice_address_get_port(&remote->addr);
	remoteInfo->type = IceTransport::NiceTypeToCandidateType(remote->type);
	remoteInfo->transportType =
	    IceTransport::NiceTransportTypeToCandidateTransportType(remote->transport);

	return true;
}

const CandidateType IceTransport::NiceTypeToCandidateType(NiceCandidateType type) {
	switch (type) {
	case NiceCandidateType::NICE_CANDIDATE_TYPE_HOST:
		return CandidateType::Host;
	case NiceCandidateType::NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
		return CandidateType::PeerReflexive;
	case NiceCandidateType::NICE_CANDIDATE_TYPE_RELAYED:
		return CandidateType::Relayed;
	case NiceCandidateType::NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
		return CandidateType::ServerReflexive;
	}
}

const CandidateTransportType
IceTransport::NiceTransportTypeToCandidateTransportType(NiceCandidateTransport type) {
	switch (type) {
	case NiceCandidateTransport::NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
		return CandidateTransportType::TcpActive;
	case NiceCandidateTransport::NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
		return CandidateTransportType::TcpPassive;
	case NiceCandidateTransport::NICE_CANDIDATE_TRANSPORT_TCP_SO:
		return CandidateTransportType::TcpSo;
	case NiceCandidateTransport::NICE_CANDIDATE_TRANSPORT_UDP:
		return CandidateTransportType::Udp;
	}
}

} // namespace rtc

#endif
