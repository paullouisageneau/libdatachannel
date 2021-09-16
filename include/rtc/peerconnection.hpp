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

#ifndef RTC_PEER_CONNECTION_H
#define RTC_PEER_CONNECTION_H

#include "candidate.hpp"
#include "common.hpp"
#include "configuration.hpp"
#include "datachannel.hpp"
#include "description.hpp"
#include "message.hpp"
#include "reliability.hpp"
#include "track.hpp"

#include <chrono>
#include <functional>

namespace rtc {

namespace impl {

struct PeerConnection;

}

struct RTC_CPP_EXPORT DataChannelInit {
	Reliability reliability = {};
	bool negotiated = false;
	optional<uint16_t> id = nullopt;
	string protocol = "";
};

class RTC_CPP_EXPORT PeerConnection final : CheshireCat<impl::PeerConnection> {
public:
	enum class State : int {
		New = RTC_NEW,
		Connecting = RTC_CONNECTING,
		Connected = RTC_CONNECTED,
		Disconnected = RTC_DISCONNECTED,
		Failed = RTC_FAILED,
		Closed = RTC_CLOSED
	};

	enum class GatheringState : int {
		New = RTC_GATHERING_NEW,
		InProgress = RTC_GATHERING_INPROGRESS,
		Complete = RTC_GATHERING_COMPLETE
	};

	enum class SignalingState : int {
		Stable = RTC_SIGNALING_STABLE,
		HaveLocalOffer = RTC_SIGNALING_HAVE_LOCAL_OFFER,
		HaveRemoteOffer = RTC_SIGNALING_HAVE_REMOTE_OFFER,
		HaveLocalPranswer = RTC_SIGNALING_HAVE_LOCAL_PRANSWER,
		HaveRemotePranswer = RTC_SIGNALING_HAVE_REMOTE_PRANSWER,
	} rtcSignalingState;

	PeerConnection();
	PeerConnection(Configuration config);
	~PeerConnection();

	void close();

	const Configuration *config() const;
	State state() const;
	GatheringState gatheringState() const;
	SignalingState signalingState() const;
	bool hasMedia() const;
	optional<Description> localDescription() const;
	optional<Description> remoteDescription() const;
	optional<string> localAddress() const;
	optional<string> remoteAddress() const;
	bool getSelectedCandidatePair(Candidate *local, Candidate *remote);

	void setLocalDescription(Description::Type type = Description::Type::Unspec);

	void setRemoteDescription(Description description);
	void addRemoteCandidate(Candidate candidate);

	shared_ptr<DataChannel> createDataChannel(string label, DataChannelInit init = {});
	void onDataChannel(std::function<void(std::shared_ptr<DataChannel> dataChannel)> callback);

	shared_ptr<Track> addTrack(Description::Media description);
	void onTrack(std::function<void(std::shared_ptr<Track> track)> callback);

	void onLocalDescription(std::function<void(Description description)> callback);
	void onLocalCandidate(std::function<void(Candidate candidate)> callback);
	void onStateChange(std::function<void(State state)> callback);
	void onGatheringStateChange(std::function<void(GatheringState state)> callback);
	void onSignalingStateChange(std::function<void(SignalingState state)> callback);

	// Stats
	void clearStats();
	size_t bytesSent();
	size_t bytesReceived();
	optional<std::chrono::milliseconds> rtt();
};

} // namespace rtc

RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, rtc::PeerConnection::State state);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out,
                                        rtc::PeerConnection::GatheringState state);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out,
                                        rtc::PeerConnection::SignalingState state);

#endif
