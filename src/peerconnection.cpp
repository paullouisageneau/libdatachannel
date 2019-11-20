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

#include "peerconnection.hpp"
#include "certificate.hpp"
#include "dtlstransport.hpp"
#include "icetransport.hpp"
#include "sctptransport.hpp"

#include <iostream>

namespace rtc {

using namespace std::placeholders;

using std::function;
using std::shared_ptr;
using std::weak_ptr;

PeerConnection::PeerConnection() : PeerConnection(Configuration()) {}

PeerConnection::PeerConnection(const Configuration &config)
    : mConfig(config), mCertificate(make_certificate("libdatachannel")), mState(State::New) {}

PeerConnection::~PeerConnection() {}

const Configuration *PeerConnection::config() const { return &mConfig; }

PeerConnection::State PeerConnection::state() const { return mState; }

PeerConnection::GatheringState PeerConnection::gatheringState() const { return mGatheringState; }

std::optional<Description> PeerConnection::localDescription() const { return mLocalDescription; }

std::optional<Description> PeerConnection::remoteDescription() const { return mRemoteDescription; }

void PeerConnection::setRemoteDescription(Description description) {
	if (!mIceTransport) {
		initIceTransport(Description::Role::ActPass);
		mIceTransport->setRemoteDescription(description);
		processLocalDescription(mIceTransport->getLocalDescription(Description::Type::Answer));
		mIceTransport->gatherLocalCandidates();
	} else {
		mIceTransport->setRemoteDescription(description);
	}

	mRemoteDescription.emplace(std::move(description));
}

void PeerConnection::addRemoteCandidate(Candidate candidate) {
	if (!mRemoteDescription || !mIceTransport)
		throw std::logic_error("Remote candidate set without remote description");

	if (mIceTransport->addRemoteCandidate(candidate))
		mRemoteDescription->addCandidate(std::move(candidate));
}

shared_ptr<DataChannel> PeerConnection::createDataChannel(const string &label,
                                                          const string &protocol,
                                                          const Reliability &reliability) {
	// The active side must use streams with even identifiers, whereas the passive side must use
	// streams with odd identifiers.
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09#section-6
	auto role = mIceTransport ? mIceTransport->role() : Description::Role::Active;
	unsigned int stream = (role == Description::Role::Active) ? 0 : 1;
	while (mDataChannels.find(stream) != mDataChannels.end()) {
		stream += 2;
		if (stream >= 65535)
			throw std::runtime_error("Too many DataChannels");
	}

	auto channel = std::make_shared<DataChannel>(stream, label, protocol, reliability);
	mDataChannels.insert(std::make_pair(stream, channel));

	if (!mIceTransport) {
		initIceTransport(Description::Role::Active);
		processLocalDescription(mIceTransport->getLocalDescription(Description::Type::Offer));
		mIceTransport->gatherLocalCandidates();
	} else if (mSctpTransport && mSctpTransport->state() == SctpTransport::State::Connected) {
		channel->open(mSctpTransport);
	}
	return channel;
}

void PeerConnection::onDataChannel(
    std::function<void(shared_ptr<DataChannel> dataChannel)> callback) {
	mDataChannelCallback = callback;
}

void PeerConnection::onLocalDescription(
    std::function<void(const Description &description)> callback) {
	mLocalDescriptionCallback = callback;
}

void PeerConnection::onLocalCandidate(std::function<void(const Candidate &candidate)> callback) {
	mLocalCandidateCallback = callback;
}

void PeerConnection::onStateChange(std::function<void(State state)> callback) {
	mStateChangeCallback = callback;
}

void PeerConnection::onGatheringStateChange(std::function<void(GatheringState state)> callback) {
	mGatheringStateChangeCallback = callback;
}

void PeerConnection::initIceTransport(Description::Role role) {
	mIceTransport = std::make_shared<IceTransport>(
	    mConfig, role, std::bind(&PeerConnection::processLocalCandidate, this, weak_ptr<PeerConnection>{shared_from_this()}, _1),
	    [this, weak_this = weak_ptr<PeerConnection>{shared_from_this()}](IceTransport::State state) {
        auto strong_this = weak_this.lock();
        if (!strong_this) return;
        
		    switch (state) {
		    case IceTransport::State::Connecting:
			    changeState(State::Connecting);
			    break;
		    case IceTransport::State::Failed:
			    changeState(State::Failed);
			    break;
		    case IceTransport::State::Ready:
			    initDtlsTransport();
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    },
	    [this, weak_this = weak_ptr<PeerConnection>{shared_from_this()}](IceTransport::GatheringState state) {
        auto strong_this = weak_this.lock();
        if (!strong_this) return;
        
		    switch (state) {
		    case IceTransport::GatheringState::InProgress:
			    changeGatheringState(GatheringState::InProgress);
			    break;
		    case IceTransport::GatheringState::Complete:
			    if (mLocalDescription)
				    mLocalDescription->endCandidates();
			    changeGatheringState(GatheringState::Complete);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    });
}

void PeerConnection::initDtlsTransport() {
	mDtlsTransport = std::make_shared<DtlsTransport>(
	    mIceTransport, mCertificate, std::bind(&PeerConnection::checkFingerprint, this, weak_ptr<PeerConnection>{shared_from_this()}, _1),
	    [this, weak_this = weak_ptr<PeerConnection>{shared_from_this()}](DtlsTransport::State state) {
        auto strong_this = weak_this.lock();
        if (!strong_this) return;
        
		    switch (state) {
		    case DtlsTransport::State::Connected:
			    initSctpTransport();
			    break;
		    case DtlsTransport::State::Failed:
			    changeState(State::Failed);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    });
}

void PeerConnection::initSctpTransport() {
	uint16_t sctpPort = mRemoteDescription->sctpPort().value_or(DEFAULT_SCTP_PORT);
	mSctpTransport = std::make_shared<SctpTransport>(
	    mDtlsTransport, sctpPort, std::bind(&PeerConnection::forwardMessage, this, weak_ptr<PeerConnection>{shared_from_this()}, _1),
	    [this, weak_this = weak_ptr<PeerConnection>{shared_from_this()}](SctpTransport::State state) {
        auto strong_this = weak_this.lock();
        if (!strong_this) return;
        
		    switch (state) {
		    case SctpTransport::State::Connected:
			    changeState(State::Connected);
			    openDataChannels();
			    break;
		    case SctpTransport::State::Failed:
			    changeState(State::Failed);
			    break;
		    case SctpTransport::State::Disconnected:
			    changeState(State::Disconnected);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    });
}

bool PeerConnection::checkFingerprint(weak_ptr<PeerConnection> weak_this, const std::string &fingerprint) const {
  auto strong_this = weak_this.lock();
  if (!strong_this) return false;
  
	if (auto expectedFingerprint =
	        mRemoteDescription ? mRemoteDescription->fingerprint() : nullopt) {
		return *expectedFingerprint == fingerprint;
	}
	return false;
}

void PeerConnection::forwardMessage(weak_ptr<PeerConnection> weak_this, message_ptr message) {
  auto strong_this = weak_this.lock();
  if (!strong_this) return;
  
	if (!mIceTransport || !mSctpTransport)
		throw std::logic_error("Got a DataChannel message without transport");

	if (!message) {
		closeDataChannels();
		return;
	}

	shared_ptr<DataChannel> channel;
	if (auto it = mDataChannels.find(message->stream); it != mDataChannels.end()) {
		channel = it->second.lock();
		if (!channel || channel->isClosed()) {
			mDataChannels.erase(it);
			channel = nullptr;
		}
	}

	if (!channel) {
		const byte dataChannelOpenMessage{0x03};
		unsigned int remoteParity = (mIceTransport->role() == Description::Role::Active) ? 1 : 0;
		if (message->type == Message::Control && *message->data() == dataChannelOpenMessage &&
		    message->stream % 2 == remoteParity) {
			channel = std::make_shared<DataChannel>(message->stream, mSctpTransport);
			channel->onOpen(std::bind(&PeerConnection::triggerDataChannel, this, weak_this, weak_ptr<DataChannel>{channel}));
			mDataChannels.insert(std::make_pair(message->stream, channel));
		} else {
			// Invalid, close the DataChannel by resetting the stream
			mSctpTransport->reset(message->stream);
			return;
		}
	}

	channel->incoming(message);
}

void PeerConnection::iterateDataChannels(
    std::function<void(shared_ptr<DataChannel> channel)> func) {
	auto it = mDataChannels.begin();
	while (it != mDataChannels.end()) {
		auto channel = it->second.lock();
		if (!channel || channel->isClosed()) {
			it = mDataChannels.erase(it);
			continue;
		}
		func(channel);
		++it;
	}
}

void PeerConnection::openDataChannels() {
	iterateDataChannels([this](shared_ptr<DataChannel> channel) { channel->open(mSctpTransport); });
}

void PeerConnection::closeDataChannels() {
	iterateDataChannels([](shared_ptr<DataChannel> channel) { channel->close(); });
}

void PeerConnection::processLocalDescription(Description description) {
	auto remoteSctpPort = mRemoteDescription ? mRemoteDescription->sctpPort() : nullopt;

	description.setFingerprint(mCertificate->fingerprint());
	description.setSctpPort(remoteSctpPort.value_or(DEFAULT_SCTP_PORT));
	mLocalDescription.emplace(std::move(description));

	if (mLocalDescriptionCallback)
		mLocalDescriptionCallback(*mLocalDescription);
}

void PeerConnection::processLocalCandidate(weak_ptr<PeerConnection> weak_this, Candidate candidate) {
  auto strong_this = weak_this.lock();
  if (!strong_this) return;
  
	if (!mLocalDescription)
		throw std::logic_error("Got a local candidate without local description");

	mLocalDescription->addCandidate(candidate);

	if (mLocalCandidateCallback)
		mLocalCandidateCallback(candidate);
}

void PeerConnection::triggerDataChannel(weak_ptr<PeerConnection> weak_this, weak_ptr<DataChannel> weakDataChannel) {
  auto strong_this = weak_this.lock();
  if (!strong_this) return;
  
  auto dataChannel = weakDataChannel.lock();
  if (!dataChannel) return;
  
	if (mDataChannelCallback)
		mDataChannelCallback(dataChannel);
}

void PeerConnection::changeState(State state) {
	mState = state;
	if (mStateChangeCallback)
		mStateChangeCallback(state);
}

void PeerConnection::changeGatheringState(GatheringState state) {
	mGatheringState = state;
	if (mGatheringStateChangeCallback)
		mGatheringStateChangeCallback(state);
}

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::PeerConnection::State &state) {
	using State = rtc::PeerConnection::State;
	std::string str;
	switch (state) {
	case State::New:
		str = "new";
		break;
	case State::Connecting:
		str = "connecting";
		break;
	case State::Connected:
		str = "connected";
		break;
	case State::Disconnected:
		str = "disconnected";
		break;
	case State::Failed:
		str = "failed";
		break;
	default:
		str = "unknown";
		break;
	}
	return out << str;
}

std::ostream &operator<<(std::ostream &out, const rtc::PeerConnection::GatheringState &state) {
	using GatheringState = rtc::PeerConnection::GatheringState;
	std::string str;
	switch (state) {
	case GatheringState::New:
		str = "new";
		break;
	case GatheringState::InProgress:
		str = "in_progress";
		break;
	case GatheringState::Complete:
		str = "complete";
		break;
	default:
		str = "unknown";
		break;
	}
	return out << str;
}

