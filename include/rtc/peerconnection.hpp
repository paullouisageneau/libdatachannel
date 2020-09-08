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
#include "configuration.hpp"
#include "datachannel.hpp"
#include "description.hpp"
#include "include.hpp"
#include "init.hpp"
#include "message.hpp"
#include "reliability.hpp"
#include "rtc.hpp"
#include "track.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace rtc {

class Certificate;
class Processor;
class IceTransport;
class DtlsTransport;
class SctpTransport;

using certificate_ptr = std::shared_ptr<Certificate>;
using future_certificate_ptr = std::shared_future<certificate_ptr>;

class PeerConnection final : public std::enable_shared_from_this<PeerConnection> {
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

	PeerConnection(void);
	PeerConnection(const Configuration &config);
	~PeerConnection();

	void close();

	const Configuration *config() const;
	State state() const;
	GatheringState gatheringState() const;
	std::optional<Description> localDescription() const;
	std::optional<Description> remoteDescription() const;
	std::optional<string> localAddress() const;
	std::optional<string> remoteAddress() const;

	void setLocalDescription();
	void setRemoteDescription(Description description);
	void addRemoteCandidate(Candidate candidate);

	std::shared_ptr<DataChannel> createDataChannel(string label, string protocol = "",
	                                               Reliability reliability = {});

	void onDataChannel(std::function<void(std::shared_ptr<DataChannel> dataChannel)> callback);
	void onLocalDescription(std::function<void(Description description)> callback);
	void onLocalCandidate(std::function<void(Candidate candidate)> callback);
	void onStateChange(std::function<void(State state)> callback);
	void onGatheringStateChange(std::function<void(GatheringState state)> callback);

	// Stats
	void clearStats();
	size_t bytesSent();
	size_t bytesReceived();
	std::optional<std::chrono::milliseconds> rtt();

	// Media support requires compilation with SRTP
	bool hasMedia() const;

	std::shared_ptr<Track> createTrack(Description::Media description);
	void onTrack(std::function<void(std::shared_ptr<Track> track)> callback);

	// libnice only
	bool getSelectedCandidatePair(CandidateInfo *local, CandidateInfo *remote);

private:
	std::shared_ptr<IceTransport> initIceTransport(Description::Role role);
	std::shared_ptr<DtlsTransport> initDtlsTransport();
	std::shared_ptr<SctpTransport> initSctpTransport();
	void closeTransports();

	void endLocalCandidates();
	bool checkFingerprint(const std::string &fingerprint) const;
	void forwardMessage(message_ptr message);
	void forwardMedia(message_ptr message);
	void forwardBufferedAmount(uint16_t stream, size_t amount);

	std::shared_ptr<DataChannel> emplaceDataChannel(Description::Role role, string label,
	                                                string protocol, Reliability reliability);
	std::shared_ptr<DataChannel> findDataChannel(uint16_t stream);
	void iterateDataChannels(std::function<void(std::shared_ptr<DataChannel> channel)> func);
	void openDataChannels();
	void closeDataChannels();
	void remoteCloseDataChannels();

	void incomingTrack(Description::Media description);
	void openTracks();

	void processLocalDescription(Description description);
	void processLocalCandidate(Candidate candidate);
	void triggerDataChannel(std::weak_ptr<DataChannel> weakDataChannel);
	void triggerTrack(std::shared_ptr<Track> track);
	bool changeState(State state);
	bool changeGatheringState(GatheringState state);

	void resetCallbacks();

	void outgoingMedia(message_ptr message);

	const init_token mInitToken = Init::Token();
	const Configuration mConfig;
	const future_certificate_ptr mCertificate;
	const std::unique_ptr<Processor> mProcessor;

	std::optional<Description> mLocalDescription, mRemoteDescription;
	mutable std::mutex mLocalDescriptionMutex, mRemoteDescriptionMutex;

	std::shared_ptr<IceTransport> mIceTransport;
	std::shared_ptr<DtlsTransport> mDtlsTransport;
	std::shared_ptr<SctpTransport> mSctpTransport;

	std::unordered_map<unsigned int, std::weak_ptr<DataChannel>> mDataChannels; // by stream ID
	std::unordered_map<string, std::weak_ptr<Track>> mTracks;                   // by mid
	std::shared_mutex mDataChannelsMutex, mTracksMutex;

	std::unordered_map<unsigned int, string> mMidFromPayloadType; // cache

	std::atomic<State> mState;
	std::atomic<GatheringState> mGatheringState;

	synchronized_callback<std::shared_ptr<DataChannel>> mDataChannelCallback;
	synchronized_callback<Description> mLocalDescriptionCallback;
	synchronized_callback<Candidate> mLocalCandidateCallback;
	synchronized_callback<State> mStateChangeCallback;
	synchronized_callback<GatheringState> mGatheringStateChangeCallback;
	synchronized_callback<std::shared_ptr<Track>> mTrackCallback;
};

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::PeerConnection::State &state);
std::ostream &operator<<(std::ostream &out, const rtc::PeerConnection::GatheringState &state);

#endif
