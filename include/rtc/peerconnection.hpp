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
#include "message.hpp"
#include "reliability.hpp"
#include "rtc.hpp"

#include <atomic>
#include <functional>
#include <unordered_map>

namespace rtc {

class Certificate;
class IceTransport;
class DtlsTransport;
class SctpTransport;

class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
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
		Complete = RTC_GATHERING_COMPLETE,
	};

	PeerConnection(void);
	PeerConnection(const Configuration &config);
	~PeerConnection();

	const Configuration *config() const;
	State state() const;
	GatheringState gatheringState() const;
	std::optional<Description> localDescription() const;
	std::optional<Description> remoteDescription() const;

	void setRemoteDescription(Description description);
	void addRemoteCandidate(Candidate candidate);

	std::shared_ptr<DataChannel> createDataChannel(const string &label, const string &protocol = "",
	                                               const Reliability &reliability = {});

	void onDataChannel(std::function<void(std::shared_ptr<DataChannel> dataChannel)> callback);
	void onLocalDescription(std::function<void(const Description &description)> callback);
	void onLocalCandidate(std::function<void(const Candidate &candidate)> callback);
	void onStateChange(std::function<void(State state)> callback);
	void onGatheringStateChange(std::function<void(GatheringState state)> callback);

private:
	void initIceTransport(Description::Role role);
	void initDtlsTransport();
	void initSctpTransport();

	bool checkFingerprint(std::weak_ptr<PeerConnection> weak_this, const std::string &fingerprint) const;
	void forwardMessage(std::weak_ptr<PeerConnection> weak_this, message_ptr message);
	void iterateDataChannels(std::function<void(std::shared_ptr<DataChannel> channel)> func);
	void openDataChannels();
	void closeDataChannels();

	void processLocalDescription(Description description);
	void processLocalCandidate(std::weak_ptr<PeerConnection> weak_this, Candidate candidate);
	void triggerDataChannel(std::weak_ptr<PeerConnection> weak_this, std::weak_ptr<DataChannel> weakDataChannel);
	void changeState(State state);
	void changeGatheringState(GatheringState state);

	const Configuration mConfig;
	const std::shared_ptr<Certificate> mCertificate;

	std::optional<Description> mLocalDescription;
	std::optional<Description> mRemoteDescription;

	std::shared_ptr<IceTransport> mIceTransport;
	std::shared_ptr<DtlsTransport> mDtlsTransport;
	std::shared_ptr<SctpTransport> mSctpTransport;

	std::unordered_map<unsigned int, std::weak_ptr<DataChannel>> mDataChannels;

	std::atomic<State> mState;
	std::atomic<GatheringState> mGatheringState;

	std::function<void(std::shared_ptr<DataChannel> dataChannel)> mDataChannelCallback;
	std::function<void(const Description &description)> mLocalDescriptionCallback;
	std::function<void(const Candidate &candidate)> mLocalCandidateCallback;
	std::function<void(State state)> mStateChangeCallback;
	std::function<void(GatheringState state)> mGatheringStateChangeCallback;
};

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::PeerConnection::State &state);
std::ostream &operator<<(std::ostream &out, const rtc::PeerConnection::GatheringState &state);

#endif
