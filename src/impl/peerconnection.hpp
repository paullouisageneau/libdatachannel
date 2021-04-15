/**
 * Copyright (c) 2019-2021 Paul-Louis Ageneau
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

#ifndef RTC_IMPL_PEER_CONNECTION_H
#define RTC_IMPL_PEER_CONNECTION_H

#include "common.hpp"
#include "datachannel.hpp"
#include "dtlstransport.hpp"
#include "icetransport.hpp"
#include "sctptransport.hpp"
#include "track.hpp"

#include "rtc/peerconnection.hpp"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace rtc::impl {

struct PeerConnection : std::enable_shared_from_this<PeerConnection> {
	using State = rtc::PeerConnection::State;
	using GatheringState = rtc::PeerConnection::GatheringState;
	using SignalingState = rtc::PeerConnection::SignalingState;

	PeerConnection(Configuration config_);
	~PeerConnection();

	void close();

	optional<Description> localDescription() const;
	optional<Description> remoteDescription() const;
	size_t remoteMaxMessageSize() const;

	shared_ptr<IceTransport> initIceTransport();
	shared_ptr<DtlsTransport> initDtlsTransport();
	shared_ptr<SctpTransport> initSctpTransport();
	shared_ptr<IceTransport> getIceTransport() const;
	shared_ptr<DtlsTransport> getDtlsTransport() const;
	shared_ptr<SctpTransport> getSctpTransport() const;
	void closeTransports();

	void endLocalCandidates();
	void rollbackLocalDescription();
	bool checkFingerprint(const std::string &fingerprint) const;
	void forwardMessage(message_ptr message);
	void forwardMedia(message_ptr message);
	void forwardBufferedAmount(uint16_t stream, size_t amount);
	optional<string> getMidFromSsrc(uint32_t ssrc);

	shared_ptr<DataChannel> emplaceDataChannel(string label, DataChannelInit init);
	shared_ptr<DataChannel> findDataChannel(uint16_t stream);
	void shiftDataChannels();
	void iterateDataChannels(std::function<void(shared_ptr<DataChannel> channel)> func);
	void openDataChannels();
	void closeDataChannels();
	void remoteCloseDataChannels();

	shared_ptr<Track> emplaceTrack(Description::Media description);
	void incomingTrack(Description::Media description);
	void openTracks();

	void validateRemoteDescription(const Description &description);
	void processLocalDescription(Description description);
	void processLocalCandidate(Candidate candidate);
	void processRemoteDescription(Description description);
	void processRemoteCandidate(Candidate candidate);
	string localBundleMid() const;

	void triggerDataChannel(weak_ptr<DataChannel> weakDataChannel);
	void triggerTrack(weak_ptr<Track> weakTrack);

	void triggerPendingDataChannels();
	void triggerPendingTracks();

	void flushPendingDataChannels();
	void flushPendingTracks();

	bool changeState(State newState);
	bool changeGatheringState(GatheringState newState);
	bool changeSignalingState(SignalingState newState);

	void resetCallbacks();

	void outgoingMedia(message_ptr message);

	const Configuration config;
	std::atomic<State> state = State::New;
	std::atomic<GatheringState> gatheringState = GatheringState::New;
	std::atomic<SignalingState> signalingState = SignalingState::Stable;
	std::atomic<bool> negotiationNeeded = false;

	synchronized_callback<shared_ptr<rtc::DataChannel>> dataChannelCallback;
	synchronized_callback<Description> localDescriptionCallback;
	synchronized_callback<Candidate> localCandidateCallback;
	synchronized_callback<State> stateChangeCallback;
	synchronized_callback<GatheringState> gatheringStateChangeCallback;
	synchronized_callback<SignalingState> signalingStateChangeCallback;
	synchronized_callback<shared_ptr<rtc::Track>> trackCallback;

private:
	const init_token mInitToken = Init::Token();
	const future_certificate_ptr mCertificate;
	const unique_ptr<Processor> mProcessor;

	optional<Description> mLocalDescription, mRemoteDescription;
	optional<Description> mCurrentLocalDescription;
	mutable std::mutex mLocalDescriptionMutex, mRemoteDescriptionMutex;

	shared_ptr<IceTransport> mIceTransport;
	shared_ptr<DtlsTransport> mDtlsTransport;
	shared_ptr<SctpTransport> mSctpTransport;

	std::unordered_map<uint16_t, weak_ptr<DataChannel>> mDataChannels; // by stream ID
	std::unordered_map<string, weak_ptr<Track>> mTracks;               // by mid
	std::vector<weak_ptr<Track>> mTrackLines;                          // by SDP order
	std::shared_mutex mDataChannelsMutex, mTracksMutex;

	Queue<shared_ptr<DataChannel>> mPendingDataChannels;
	Queue<shared_ptr<Track>> mPendingTracks;

	std::unordered_map<uint32_t, string> mMidFromSsrc; // cache
};

} // namespace rtc::impl

#endif
