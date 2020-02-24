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
#include "include.hpp"
#include "sctptransport.hpp"

#include <iostream>

namespace rtc {

using namespace std::placeholders;

using std::shared_ptr;
using std::weak_ptr;

PeerConnection::PeerConnection() : PeerConnection(Configuration()) {}

PeerConnection::PeerConnection(const Configuration &config)
    : mConfig(config), mCertificate(make_certificate("libdatachannel")), mState(State::New) {}

PeerConnection::~PeerConnection() {
	changeState(State::Destroying);
	close();
	mSctpTransport.reset();
	mDtlsTransport.reset();
	mIceTransport.reset();
}

void PeerConnection::close() {
	// Close DataChannels
	closeDataChannels();
	mDataChannels.clear();

	// Close Transports
	for (int i = 0; i < 2; ++i) { // Make sure a transport wasn't spawn behind our back
		if (auto transport = std::atomic_load(&mSctpTransport))
			transport->stop();
		if (auto transport = std::atomic_load(&mDtlsTransport))
			transport->stop();
		if (auto transport = std::atomic_load(&mIceTransport))
			transport->stop();
	}
	changeState(State::Closed);
}

const Configuration *PeerConnection::config() const { return &mConfig; }

PeerConnection::State PeerConnection::state() const { return mState; }

PeerConnection::GatheringState PeerConnection::gatheringState() const { return mGatheringState; }

std::optional<Description> PeerConnection::localDescription() const {
	std::lock_guard lock(mLocalDescriptionMutex);
	return mLocalDescription;
}

std::optional<Description> PeerConnection::remoteDescription() const {
	std::lock_guard lock(mRemoteDescriptionMutex);
	return mRemoteDescription;
}

void PeerConnection::setRemoteDescription(Description description) {
	description.hintType(localDescription() ? Description::Type::Answer : Description::Type::Offer);
	auto remoteCandidates = description.extractCandidates();

	std::lock_guard lock(mRemoteDescriptionMutex);
	mRemoteDescription.emplace(std::move(description));

	auto iceTransport = std::atomic_load(&mIceTransport);
	if (!iceTransport)
		iceTransport = initIceTransport(Description::Role::ActPass);

	iceTransport->setRemoteDescription(*mRemoteDescription);

	if (mRemoteDescription->type() == Description::Type::Offer) {
		// This is an offer and we are the answerer.
		processLocalDescription(iceTransport->getLocalDescription(Description::Type::Answer));
		iceTransport->gatherLocalCandidates();
	} else {
		// This is an answer and we are the offerer.
		auto sctpTransport = std::atomic_load(&mSctpTransport);
		if (!sctpTransport && iceTransport->role() == Description::Role::Active) {
			// Since we assumed passive role during DataChannel creation, we need to shift the
			// stream numbers by one to shift them from odd to even.
			decltype(mDataChannels) newDataChannels;
			iterateDataChannels([&](shared_ptr<DataChannel> channel) {
				if (channel->stream() % 2 == 1)
					channel->mStream -= 1;
				newDataChannels.emplace(channel->stream(), channel);
			});
			std::swap(mDataChannels, newDataChannels);
		}
	}

	for (const auto &candidate : remoteCandidates)
		addRemoteCandidate(candidate);
}

void PeerConnection::addRemoteCandidate(Candidate candidate) {
	std::lock_guard lock(mRemoteDescriptionMutex);

	auto iceTransport = std::atomic_load(&mIceTransport);
	if (!mRemoteDescription || !iceTransport)
		throw std::logic_error("Remote candidate set without remote description");

	mRemoteDescription->addCandidate(candidate);

	if (candidate.resolve(Candidate::ResolveMode::Simple)) {
		iceTransport->addRemoteCandidate(candidate);
	} else {
		// OK, we might need a lookup, do it asynchronously
		weak_ptr<IceTransport> weakIceTransport{iceTransport};
		std::thread t([weakIceTransport, candidate]() mutable {
			if (candidate.resolve(Candidate::ResolveMode::Lookup))
				if (auto iceTransport = weakIceTransport.lock())
					iceTransport->addRemoteCandidate(candidate);
		});
		t.detach();
	}
}

std::optional<string> PeerConnection::localAddress() const {
	auto iceTransport = std::atomic_load(&mIceTransport);
	return iceTransport ? iceTransport->getLocalAddress() : nullopt;
}

std::optional<string> PeerConnection::remoteAddress() const {
	auto iceTransport = std::atomic_load(&mIceTransport);
	return iceTransport ? iceTransport->getRemoteAddress() : nullopt;
}

shared_ptr<DataChannel> PeerConnection::createDataChannel(const string &label,
                                                          const string &protocol,
                                                          const Reliability &reliability) {
	// RFC 5763: The answerer MUST use either a setup attribute value of setup:active or
	// setup:passive. [...] Thus, setup:active is RECOMMENDED.
	// See https://tools.ietf.org/html/rfc5763#section-5
	// Therefore, we assume passive role when we are the offerer.
	auto iceTransport = std::atomic_load(&mIceTransport);
	auto role = iceTransport ? iceTransport->role() : Description::Role::Passive;

	// The active side must use streams with even identifiers, whereas the passive side must use
	// streams with odd identifiers.
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09#section-6
	unsigned int stream = (role == Description::Role::Active) ? 0 : 1;
	while (mDataChannels.find(stream) != mDataChannels.end()) {
		stream += 2;
		if (stream >= 65535)
			throw std::runtime_error("Too many DataChannels");
	}

	auto channel =
	    std::make_shared<DataChannel>(shared_from_this(), stream, label, protocol, reliability);
	mDataChannels.insert(std::make_pair(stream, channel));

	if (!iceTransport) {
		// RFC 5763: The endpoint that is the offerer MUST use the setup attribute value of
		// setup:actpass.
		// See https://tools.ietf.org/html/rfc5763#section-5
		iceTransport = initIceTransport(Description::Role::ActPass);
		processLocalDescription(iceTransport->getLocalDescription(Description::Type::Offer));
		iceTransport->gatherLocalCandidates();
	} else {
		if (auto transport = std::atomic_load(&mSctpTransport))
			if (transport->state() == SctpTransport::State::Connected)
				channel->open(transport);
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

shared_ptr<IceTransport> PeerConnection::initIceTransport(Description::Role role) {
	std::lock_guard lock(mInitMutex);
	if (auto transport = std::atomic_load(&mIceTransport))
		return transport;

	auto transport = std::make_shared<IceTransport>(
	    mConfig, role, std::bind(&PeerConnection::processLocalCandidate, this, _1),
	    [this](IceTransport::State state) {
		    switch (state) {
		    case IceTransport::State::Connecting:
			    changeState(State::Connecting);
			    break;
		    case IceTransport::State::Failed:
			    changeState(State::Failed);
			    break;
		    case IceTransport::State::Connected:
			    initDtlsTransport();
			    break;
		    case IceTransport::State::Disconnected:
			    changeState(State::Disconnected);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    },
	    [this](IceTransport::GatheringState state) {
		    switch (state) {
		    case IceTransport::GatheringState::InProgress:
			    changeGatheringState(GatheringState::InProgress);
			    break;
		    case IceTransport::GatheringState::Complete:
			    endLocalCandidates();
			    changeGatheringState(GatheringState::Complete);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    });
	std::atomic_store(&mIceTransport, transport);
	return transport;
}

shared_ptr<DtlsTransport> PeerConnection::initDtlsTransport() {
	std::lock_guard lock(mInitMutex);
	if (auto transport = std::atomic_load(&mDtlsTransport))
		return transport;

	auto lower = std::atomic_load(&mIceTransport);
	auto transport = std::make_shared<DtlsTransport>(
	    lower, mCertificate, std::bind(&PeerConnection::checkFingerprint, this, _1),
	    [this](DtlsTransport::State state) {
		    switch (state) {
		    case DtlsTransport::State::Connected:
			    initSctpTransport();
			    break;
		    case DtlsTransport::State::Failed:
			    changeState(State::Failed);
			    break;
		    case DtlsTransport::State::Disconnected:
			    changeState(State::Disconnected);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    });
	std::atomic_store(&mDtlsTransport, transport);
	return transport;
}

shared_ptr<SctpTransport> PeerConnection::initSctpTransport() {
	std::lock_guard lock(mInitMutex);
	if (auto transport = std::atomic_load(&mSctpTransport))
		return transport;

	uint16_t sctpPort = remoteDescription()->sctpPort().value_or(DEFAULT_SCTP_PORT);
	auto lower = std::atomic_load(&mDtlsTransport);
	auto transport = std::make_shared<SctpTransport>(
	    lower, sctpPort, std::bind(&PeerConnection::forwardMessage, this, _1),
	    std::bind(&PeerConnection::forwardBufferedAmount, this, _1, _2),
	    [this](SctpTransport::State state) {
		    switch (state) {
		    case SctpTransport::State::Connected:
			    changeState(State::Connected);
			    openDataChannels();
			    break;
		    case SctpTransport::State::Failed:
			    remoteCloseDataChannels();
			    changeState(State::Failed);
			    break;
		    case SctpTransport::State::Disconnected:
			    remoteCloseDataChannels();
			    changeState(State::Disconnected);
			    break;
		    default:
			    // Ignore
			    break;
		    }
	    });
	std::atomic_store(&mSctpTransport, transport);
	return transport;
}

void PeerConnection::endLocalCandidates() {
	std::lock_guard lock(mLocalDescriptionMutex);
	if (mLocalDescription)
		mLocalDescription->endCandidates();
}

bool PeerConnection::checkFingerprint(const std::string &fingerprint) const {
	std::lock_guard lock(mRemoteDescriptionMutex);
	if (auto expectedFingerprint =
	        mRemoteDescription ? mRemoteDescription->fingerprint() : nullopt) {
		return *expectedFingerprint == fingerprint;
	}
	return false;
}

void PeerConnection::forwardMessage(message_ptr message) {
	if (!message) {
		remoteCloseDataChannels();
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

	auto iceTransport = std::atomic_load(&mIceTransport);
	auto sctpTransport = std::atomic_load(&mSctpTransport);
	if (!iceTransport || !sctpTransport)
		return;

	if (!channel) {
		const byte dataChannelOpenMessage{0x03};
		unsigned int remoteParity = (iceTransport->role() == Description::Role::Active) ? 1 : 0;
		if (message->type == Message::Control && *message->data() == dataChannelOpenMessage &&
		    message->stream % 2 == remoteParity) {
			channel =
			    std::make_shared<DataChannel>(shared_from_this(), sctpTransport, message->stream);
			channel->onOpen(std::bind(&PeerConnection::triggerDataChannel, this,
			                          weak_ptr<DataChannel>{channel}));
			mDataChannels.insert(std::make_pair(message->stream, channel));
		} else {
			// Invalid, close the DataChannel by resetting the stream
			sctpTransport->reset(message->stream);
			return;
		}
	}

	channel->incoming(message);
}

void PeerConnection::forwardBufferedAmount(uint16_t stream, size_t amount) {
	shared_ptr<DataChannel> channel;
	if (auto it = mDataChannels.find(stream); it != mDataChannels.end()) {
		channel = it->second.lock();
		if (!channel || channel->isClosed()) {
			mDataChannels.erase(it);
			channel = nullptr;
		}
	}

	if (channel)
		channel->triggerBufferedAmount(amount);
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
	if (auto transport = std::atomic_load(&mSctpTransport))
		iterateDataChannels([&](shared_ptr<DataChannel> channel) { channel->open(transport); });
}

void PeerConnection::closeDataChannels() {
	iterateDataChannels([&](shared_ptr<DataChannel> channel) { channel->close(); });
}

void PeerConnection::remoteCloseDataChannels() {
	iterateDataChannels([&](shared_ptr<DataChannel> channel) { channel->remoteClose(); });
}

void PeerConnection::processLocalDescription(Description description) {
	std::optional<uint16_t> remoteSctpPort;
	if (auto remote = remoteDescription())
		remoteSctpPort = remote->sctpPort();

	std::lock_guard lock(mLocalDescriptionMutex);
	mLocalDescription.emplace(std::move(description));
	mLocalDescription->setFingerprint(mCertificate->fingerprint());
	mLocalDescription->setSctpPort(remoteSctpPort.value_or(DEFAULT_SCTP_PORT));
	mLocalDescription->setMaxMessageSize(LOCAL_MAX_MESSAGE_SIZE);

	mLocalDescriptionCallback(*mLocalDescription);
}

void PeerConnection::processLocalCandidate(Candidate candidate) {
	std::lock_guard lock(mLocalDescriptionMutex);
	if (!mLocalDescription)
		throw std::logic_error("Got a local candidate without local description");

	mLocalDescription->addCandidate(candidate);

	mLocalCandidateCallback(candidate);
}

void PeerConnection::triggerDataChannel(weak_ptr<DataChannel> weakDataChannel) {
	auto dataChannel = weakDataChannel.lock();
	if (!dataChannel)
		return;

	mDataChannelCallback(dataChannel);
}

void PeerConnection::changeState(State state) {
	State current;
	do {
		current = mState.load();
		if (current == state || current == State::Destroying)
			return;
	} while (!mState.compare_exchange_weak(current, state));

	if (state != State::Destroying)
		mStateChangeCallback(state);
}

void PeerConnection::changeGatheringState(GatheringState state) {
	if (mGatheringState.exchange(state) != state)
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
	case State::Closed:
		str = "closed";
		break;
	case State::Destroying:
		str = "destroying";
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

