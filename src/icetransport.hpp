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

#ifndef RTC_ICE_TRANSPORT_H
#define RTC_ICE_TRANSPORT_H

#include "candidate.hpp"
#include "configuration.hpp"
#include "description.hpp"
#include "include.hpp"
#include "peerconnection.hpp"
#include "transport.hpp"

#if !USE_NICE
#include <juice/juice.h>
#else
#include <nice/agent.h>
#endif

#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

namespace rtc {

class IceTransport : public Transport {
public:
	enum class GatheringState { New = 0, InProgress = 1, Complete = 2 };

	using candidate_callback = std::function<void(const Candidate &candidate)>;
	using gathering_state_callback = std::function<void(GatheringState state)>;

	IceTransport(const Configuration &config, candidate_callback candidateCallback,
	             state_callback stateChangeCallback,
	             gathering_state_callback gatheringStateChangeCallback);
	~IceTransport();

	Description::Role role() const;
	GatheringState gatheringState() const;
	Description getLocalDescription(Description::Type type) const;
	void setRemoteDescription(const Description &description);
	bool addRemoteCandidate(const Candidate &candidate);
	void gatherLocalCandidates(string mid);

	std::optional<string> getLocalAddress() const;
	std::optional<string> getRemoteAddress() const;

	bool stop() override;
	bool send(message_ptr message) override; // false if dropped

	bool getSelectedCandidatePair(Candidate *local, Candidate *remote);

private:
	bool outgoing(message_ptr message) override;

	void changeGatheringState(GatheringState state);

	void processStateChange(unsigned int state);
	void processCandidate(const string &candidate);
	void processGatheringDone();
	void processTimeout();

	Description::Role mRole;
	string mMid;
	std::chrono::milliseconds mTrickleTimeout;
	std::atomic<GatheringState> mGatheringState;

	candidate_callback mCandidateCallback;
	gathering_state_callback mGatheringStateChangeCallback;

#if !USE_NICE
	std::unique_ptr<juice_agent_t, void (*)(juice_agent_t *)> mAgent;

	static void StateChangeCallback(juice_agent_t *agent, juice_state_t state, void *user_ptr);
	static void CandidateCallback(juice_agent_t *agent, const char *sdp, void *user_ptr);
	static void GatheringDoneCallback(juice_agent_t *agent, void *user_ptr);
	static void RecvCallback(juice_agent_t *agent, const char *data, size_t size, void *user_ptr);
	static void LogCallback(juice_log_level_t level, const char *message);
#else
	uint32_t mStreamId = 0;
	std::unique_ptr<NiceAgent, void (*)(gpointer)> mNiceAgent;
	std::unique_ptr<GMainLoop, void (*)(GMainLoop *)> mMainLoop;
	std::thread mMainLoopThread;
	guint mTimeoutId = 0;
	std::mutex mOutgoingMutex;
	unsigned int mOutgoingDscp;

	static string AddressToString(const NiceAddress &addr);

	static void CandidateCallback(NiceAgent *agent, NiceCandidate *candidate, gpointer userData);
	static void GatheringDoneCallback(NiceAgent *agent, guint streamId, gpointer userData);
	static void StateChangeCallback(NiceAgent *agent, guint streamId, guint componentId,
	                                guint state, gpointer userData);
	static void RecvCallback(NiceAgent *agent, guint stream_id, guint component_id, guint len,
	                         gchar *buf, gpointer userData);
	static gboolean TimeoutCallback(gpointer userData);
	static void LogCallback(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message,
	                        gpointer user_data);
#endif
};

} // namespace rtc

#endif
