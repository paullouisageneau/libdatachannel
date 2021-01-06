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
using std::chrono::system_clock;

#if !USE_NICE

#define MAX_TURN_SERVERS_COUNT 2

namespace rtc {

IceTransport::IceTransport(const Configuration &config,
                           candidate_callback candidateCallback, filter_candidate_callback filterCandidateCallback,
                           state_callback stateChangeCallback,
                           gathering_state_callback gatheringStateChangeCallback)
    : Transport(nullptr, std::move(stateChangeCallback)), mRole(Description::Role::ActPass),
      mMid("0"), mGatheringState(GatheringState::New),
      mCandidateCallback(std::move(candidateCallback)),
      mFilterCandidateCallback(std::move(filterCandidateCallback)),
      mGatheringStateChangeCallback(std::move(gatheringStateChangeCallback)),
      mAgent(nullptr, nullptr) {

	PLOG_DEBUG << "Initializing ICE transport (libjuice)";
	if (config.enableIceTcp) {
		PLOG_WARNING << "ICE-TCP is not supported with libjuice";
	}

	juice_log_level_t level;
	auto logger = plog::get();
	switch (logger ? logger->getMaxSeverity() : plog::none) {
	case plog::none:
		level = JUICE_LOG_LEVEL_NONE;
		break;
	case plog::fatal:
		level = JUICE_LOG_LEVEL_VERBOSE;
		break;
	case plog::error:
		level = JUICE_LOG_LEVEL_ERROR;
		break;
	case plog::warning:
		level = JUICE_LOG_LEVEL_WARN;
		break;
	case plog::info:
	case plog::debug: // juice debug is output as verbose
		level = JUICE_LOG_LEVEL_INFO;
		break;
	default:
		level = JUICE_LOG_LEVEL_VERBOSE;
		break;
	}
	juice_set_log_handler(IceTransport::LogCallback);
	juice_set_log_level(level);

	juice_config_t jconfig = {};
	jconfig.cb_state_changed = IceTransport::StateChangeCallback;
	jconfig.cb_candidate = IceTransport::CandidateCallback;
	jconfig.cb_gathering_done = IceTransport::GatheringDoneCallback;
	jconfig.cb_recv = IceTransport::RecvCallback;
	jconfig.cb_filter_candidate = IceTransport::FilterCandidateCallback;
	jconfig.user_ptr = this;

	// Randomize servers order
	std::vector<IceServer> servers = config.iceServers;
	auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
	std::shuffle(servers.begin(), servers.end(), std::default_random_engine(seed));

	// Pick a STUN server
	for (auto &server : servers) {
		if (!server.hostname.empty() && server.type == IceServer::Type::Stun) {
			if (server.service.empty())
				server.service = "3478"; // STUN UDP port
			PLOG_INFO << "Using STUN server \"" << server.hostname << ":" << server.service << "\"";
			jconfig.stun_server_host = server.hostname.c_str();
			jconfig.stun_server_port = uint16_t(std::stoul(server.service));
			break;
		}
	}

	juice_turn_server_t turn_servers[MAX_TURN_SERVERS_COUNT];
	std::memset(turn_servers, 0, sizeof(turn_servers));

	// Add TURN servers
	int k = 0;
	for (auto &server : servers) {
		if (!server.hostname.empty() && server.type == IceServer::Type::Turn) {
			if (server.service.empty())
				server.service = "3478"; // TURN UDP port
			PLOG_INFO << "Using TURN server \"" << server.hostname << ":" << server.service << "\"";
			turn_servers[k].host = server.hostname.c_str();
			turn_servers[k].username = server.username.c_str();
			turn_servers[k].password = server.password.c_str();
			turn_servers[k].port = uint16_t(std::stoul(server.service));
			if (++k >= MAX_TURN_SERVERS_COUNT)
				break;
		}
	}
	jconfig.turn_servers = k > 0 ? turn_servers : nullptr;
	jconfig.turn_servers_count = k;

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

IceTransport::~IceTransport() {
	stop();
	mAgent.reset();
}

bool IceTransport::stop() { return Transport::stop(); }

Description::Role IceTransport::role() const { return mRole; }

Description IceTransport::getLocalDescription(Description::Type type) const {
	char sdp[JUICE_MAX_SDP_STRING_LEN];
	if (juice_get_local_description(mAgent.get(), sdp, JUICE_MAX_SDP_STRING_LEN) < 0)
		throw std::runtime_error("Failed to generate local SDP");

	// RFC 5763: The endpoint that is the offerer MUST use the setup attribute value of
	// setup:actpass.
	// See https://tools.ietf.org/html/rfc5763#section-5
	return Description(string(sdp), type,
	                   type == Description::Type::Offer ? Description::Role::ActPass : mRole);
}

void IceTransport::setRemoteDescription(const Description &description) {
	if (mRole == Description::Role::ActPass)
		mRole = description.role() == Description::Role::Active ? Description::Role::Passive
		                                                        : Description::Role::Active;
	if (mRole == description.role())
		throw std::logic_error("Incompatible roles with remote description");

	mMid = description.bundleMid();
	if (juice_set_remote_description(mAgent.get(),
	                                 description.generateApplicationSdp("\r\n").c_str()) < 0)
		throw std::runtime_error("Failed to parse ICE settings from remote SDP");
}

bool IceTransport::addRemoteCandidate(const Candidate &candidate) {
	// Don't try to pass unresolved candidates for more safety
	if (!candidate.isResolved())
		return false;

	return juice_add_remote_candidate(mAgent.get(), string(candidate).c_str()) >= 0;
}

void IceTransport::gatherLocalCandidates(string mid) {
	mMid = std::move(mid);

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

bool IceTransport::getSelectedCandidatePair(Candidate *local, Candidate *remote) {
	char sdpLocal[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
	char sdpRemote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
	if (juice_get_selected_candidates(mAgent.get(), sdpLocal, JUICE_MAX_CANDIDATE_SDP_STRING_LEN,
	                                  sdpRemote, JUICE_MAX_CANDIDATE_SDP_STRING_LEN) == 0) {
		if (local) {
			*local = Candidate(sdpLocal, mMid);
			local->resolve(Candidate::ResolveMode::Simple);
		}
		if (remote) {
			*remote = Candidate(sdpRemote, mMid);
			remote->resolve(Candidate::ResolveMode::Simple);
		}
		return true;
	}
	return false;
}

bool IceTransport::send(message_ptr message) {
	auto s = state();
	if (!message || (s != State::Connected && s != State::Completed))
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();
	return outgoing(message);
}

bool IceTransport::outgoing(message_ptr message) {
	// Explicit Congestion Notification takes the least-significant 2 bits of the DS field
	int ds = int(message->dscp << 2);
	return juice_send_diffserv(mAgent.get(), reinterpret_cast<const char *>(message->data()),
	                           message->size(), ds) >= 0;
}

void IceTransport::changeGatheringState(GatheringState state) {
	if (mGatheringState.exchange(state) != state)
		mGatheringStateChangeCallback(mGatheringState);
}

bool IceTransport::filterCandidate(const std::string& sdp) {
    Candidate candidate(sdp);
    if (mFilterCandidateCallback) {
        auto res = mFilterCandidateCallback(candidate);
        return res;
    }
    return true;
}

void IceTransport::processStateChange(unsigned int state) {
	switch (state) {
	case JUICE_STATE_DISCONNECTED:
		changeState(State::Disconnected);
		break;
	case JUICE_STATE_CONNECTING:
		changeState(State::Connecting);
		break;
	case JUICE_STATE_CONNECTED:
		changeState(State::Connected);
		break;
	case JUICE_STATE_COMPLETED:
		changeState(State::Completed);
		break;
	case JUICE_STATE_FAILED:
		changeState(State::Failed);
		break;
	};
}

void IceTransport::processCandidate(const string &candidate) {
	mCandidateCallback(Candidate(candidate, mMid));
}

void IceTransport::processGatheringDone() { changeGatheringState(GatheringState::Complete); }

void IceTransport::StateChangeCallback(juice_agent_t *, juice_state_t state, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->processStateChange(static_cast<unsigned int>(state));
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::CandidateCallback(juice_agent_t *, const char *sdp, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->processCandidate(sdp);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::GatheringDoneCallback(juice_agent_t *, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		iceTransport->processGatheringDone();
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::RecvCallback(juice_agent_t *, const char *data, size_t size, void *user_ptr) {
	auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
	try {
		PLOG_VERBOSE << "Incoming size=" << size;
		auto b = reinterpret_cast<const byte *>(data);
		iceTransport->incoming(make_message(b, b + size));
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

bool IceTransport::FilterCandidateCallback(juice_agent_t *, const char* sdp, void *user_ptr) {
    auto iceTransport = static_cast<rtc::IceTransport *>(user_ptr);
    try {
        std::string sdpStr(sdp);
        return iceTransport->filterCandidate(sdpStr);
    } catch (const std::exception& e) {
        PLOG_WARNING << e.what();
    }
    return false;
}

} // namespace rtc

#else // USE_NICE == 1

namespace rtc {

IceTransport::IceTransport(const Configuration &config, candidate_callback candidateCallback,
                           state_callback stateChangeCallback,
                           gathering_state_callback gatheringStateChangeCallback)
    : Transport(nullptr, std::move(stateChangeCallback)), mRole(Description::Role::ActPass),
      mMid("0"), mGatheringState(GatheringState::New),
      mCandidateCallback(std::move(candidateCallback)),
      mGatheringStateChangeCallback(std::move(gatheringStateChangeCallback)),
      mNiceAgent(nullptr, nullptr), mMainLoop(nullptr, nullptr), mOutgoingDscp(0) {

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
	auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
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
		if (getaddrinfo(server.hostname.c_str(), server.service.c_str(), &hints, &result) != 0) {
			PLOG_WARNING << "Unable to resolve STUN server address: " << server.hostname << ':'
			             << server.service;
			continue;
		}

		for (auto p = result; p; p = p->ai_next) {
			if (p->ai_family == AF_INET) {
				char nodebuffer[MAX_NUMERICNODE_LEN];
				char servbuffer[MAX_NUMERICSERV_LEN];
				if (getnameinfo(p->ai_addr, p->ai_addrlen, nodebuffer, MAX_NUMERICNODE_LEN,
				                servbuffer, MAX_NUMERICNODE_LEN,
				                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
					PLOG_INFO << "Using STUN server \"" << server.hostname << ":" << server.service
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
		if (getaddrinfo(server.hostname.c_str(), server.service.c_str(), &hints, &result) != 0) {
			PLOG_WARNING << "Unable to resolve TURN server address: " << server.hostname << ':'
			             << server.service;
			continue;
		}

		for (auto p = result; p; p = p->ai_next) {
			if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
				char nodebuffer[MAX_NUMERICNODE_LEN];
				char servbuffer[MAX_NUMERICSERV_LEN];
				if (getnameinfo(p->ai_addr, p->ai_addrlen, nodebuffer, MAX_NUMERICNODE_LEN,
				                servbuffer, MAX_NUMERICNODE_LEN,
				                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
					PLOG_INFO << "Using TURN server \"" << server.hostname << ":" << server.service
					          << "\"";
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
	nice_agent_attach_recv(mNiceAgent.get(), mStreamId, 1, g_main_loop_get_context(mMainLoop.get()),
	                       NULL, NULL);
	nice_agent_remove_stream(mNiceAgent.get(), mStreamId);
	g_main_loop_quit(mMainLoop.get());
	mMainLoopThread.join();
	return true;
}

Description::Role IceTransport::role() const { return mRole; }

Description IceTransport::getLocalDescription(Description::Type type) const {
	// RFC 8445: The initiating agent that started the ICE processing MUST take the controlling
	// role, and the other MUST take the controlled role.
	g_object_set(G_OBJECT(mNiceAgent.get()), "controlling-mode",
	             type == Description::Type::Offer ? TRUE : FALSE, nullptr);

	std::unique_ptr<gchar[], void (*)(void *)> sdp(nice_agent_generate_local_sdp(mNiceAgent.get()),
	                                               g_free);

	// RFC 5763: The endpoint that is the offerer MUST use the setup attribute value of
	// setup:actpass.
	// See https://tools.ietf.org/html/rfc5763#section-5
	return Description(string(sdp.get()), type,
	                   type == Description::Type::Offer ? Description::Role::ActPass : mRole);
}

void IceTransport::setRemoteDescription(const Description &description) {
	if (mRole == Description::Role::ActPass)
		mRole = description.role() == Description::Role::Active ? Description::Role::Passive
		                                                        : Description::Role::Active;
	if (mRole == description.role())
		throw std::logic_error("Incompatible roles with remote description");

	mMid = description.bundleMid();
	mTrickleTimeout = !description.ended() ? 30s : 0s;

	// Warning: libnice expects "\n" as end of line
	if (nice_agent_parse_remote_sdp(mNiceAgent.get(),
	                                description.generateApplicationSdp("\n").c_str()) < 0)
		throw std::runtime_error("Failed to parse ICE settings from remote SDP");
}

bool IceTransport::addRemoteCandidate(const Candidate &candidate) {
	// Don't try to pass unresolved candidates to libnice for more safety
	if (!candidate.isResolved())
		return false;

	// Warning: the candidate string must start with "a=candidate:" and it must not end with a
	// newline or whitespace, else libnice will reject it.
	string sdp(candidate);
	NiceCandidate *cand =
	    nice_agent_parse_remote_candidate_sdp(mNiceAgent.get(), mStreamId, sdp.c_str());
	if (!cand) {
		PLOG_WARNING << "Rejected ICE candidate: " << sdp;
		return false;
	}

	GSList *list = g_slist_append(nullptr, cand);
	int ret = nice_agent_set_remote_candidates(mNiceAgent.get(), mStreamId, 1, list);

	g_slist_free_full(list, reinterpret_cast<GDestroyNotify>(nice_candidate_free));
	return ret > 0;
}

void IceTransport::gatherLocalCandidates(string mid) {
	mMid = std::move(mid);

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
	auto s = state();
	if (!message || (s != State::Connected && s != State::Completed))
		return false;

	PLOG_VERBOSE << "Send size=" << message->size();
	return outgoing(message);
}

bool IceTransport::outgoing(message_ptr message) {
	std::lock_guard lock(mOutgoingMutex);
	if (mOutgoingDscp != message->dscp) {
		mOutgoingDscp = message->dscp;
		// Explicit Congestion Notification takes the least-significant 2 bits of the DS field
		int ds = int(message->dscp << 2);
		nice_agent_set_stream_tos(mNiceAgent.get(), mStreamId, ds); // ToS is the legacy name for DS
	}
	return nice_agent_send(mNiceAgent.get(), mStreamId, 1, message->size(),
	                       reinterpret_cast<const char *>(message->data())) >= 0;
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

	switch (state) {
	case NICE_COMPONENT_STATE_DISCONNECTED:
		changeState(State::Disconnected);
		break;
	case NICE_COMPONENT_STATE_CONNECTING:
		changeState(State::Connecting);
		break;
	case NICE_COMPONENT_STATE_CONNECTED:
		changeState(State::Connected);
		break;
	case NICE_COMPONENT_STATE_READY:
		changeState(State::Completed);
		break;
	case NICE_COMPONENT_STATE_FAILED:
		changeState(State::Failed);
		break;
	};
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

void IceTransport::GatheringDoneCallback(NiceAgent * /*agent*/, guint /*streamId*/,
                                         gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processGatheringDone();
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::StateChangeCallback(NiceAgent * /*agent*/, guint /*streamId*/,
                                       guint /*componentId*/, guint state, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		iceTransport->processStateChange(state);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void IceTransport::RecvCallback(NiceAgent * /*agent*/, guint /*streamId*/, guint /*componentId*/,
                                guint len, gchar *buf, gpointer userData) {
	auto iceTransport = static_cast<rtc::IceTransport *>(userData);
	try {
		PLOG_VERBOSE << "Incoming size=" << len;
		auto b = reinterpret_cast<byte *>(buf);
		iceTransport->incoming(make_message(b, b + len));
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

void IceTransport::LogCallback(const gchar * /*logDomain*/, GLogLevelFlags logLevel,
                               const gchar *message, gpointer /*userData*/) {
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

bool IceTransport::getSelectedCandidatePair(Candidate *local, Candidate *remote) {
	NiceCandidate *niceLocal, *niceRemote;
	if (!nice_agent_get_selected_pair(mNiceAgent.get(), mStreamId, 1, &niceLocal, &niceRemote))
		return false;

	gchar *sdpLocal = nice_agent_generate_local_candidate_sdp(mNiceAgent.get(), niceLocal);
	if (local)
		*local = Candidate(sdpLocal, mMid);
	g_free(sdpLocal);

	gchar *sdpRemote = nice_agent_generate_local_candidate_sdp(mNiceAgent.get(), niceRemote);
	if (remote)
		*remote = Candidate(sdpRemote, mMid);
	g_free(sdpRemote);

	if (local)
		local->resolve(Candidate::ResolveMode::Simple);
	if (remote)
		remote->resolve(Candidate::ResolveMode::Simple);
	return true;
}

} // namespace rtc

#endif
