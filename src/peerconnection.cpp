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

PeerConnection::PeerConnection() : PeerConnection(Configuration()) {}

PeerConnection::PeerConnection(const Configuration &config)
    : mConfig(config), mCertificate(make_certificate("libdatachannel")) {}

PeerConnection::~PeerConnection() {}

const Configuration *PeerConnection::config() const { return &mConfig; }

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
		mRemoteDescription->addCandidate(std::make_optional(std::move(candidate)));
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
	} else if (mSctpTransport && mSctpTransport->isReady()) {
		channel->open(mSctpTransport);
	}
	return channel;
}

void PeerConnection::onDataChannel(
    std::function<void(std::shared_ptr<DataChannel> dataChannel)> callback) {
	mDataChannelCallback = callback;
}

void PeerConnection::onLocalDescription(
    std::function<void(const Description &description)> callback) {
	mLocalDescriptionCallback = callback;
}

void PeerConnection::onLocalCandidate(
    std::function<void(const std::optional<Candidate> &candidate)> callback) {
	mLocalCandidateCallback = callback;
}

void PeerConnection::initIceTransport(Description::Role role) {
	mIceTransport = std::make_shared<IceTransport>(
	    mConfig, role, std::bind(&PeerConnection::processLocalCandidate, this, _1),
	    std::bind(&PeerConnection::initDtlsTransport, this));
}

void PeerConnection::initDtlsTransport() {
	mDtlsTransport = std::make_shared<DtlsTransport>(
	    mIceTransport, mCertificate, std::bind(&PeerConnection::checkFingerprint, this, _1),
	    std::bind(&PeerConnection::initSctpTransport, this));
}

void PeerConnection::initSctpTransport() {
	uint16_t sctpPort = mRemoteDescription->sctpPort().value_or(DEFAULT_SCTP_PORT);
	mSctpTransport = std::make_shared<SctpTransport>(
	    mDtlsTransport, sctpPort, std::bind(&PeerConnection::openDataChannels, this),
	    std::bind(&PeerConnection::forwardMessage, this, _1));
}

bool PeerConnection::checkFingerprint(const std::string &fingerprint) const {
	if (auto expectedFingerprint =
	        mRemoteDescription ? mRemoteDescription->fingerprint() : nullopt) {
		return *expectedFingerprint == fingerprint;
	}
	return false;
}

void PeerConnection::forwardMessage(message_ptr message) {
	if (!mIceTransport || !mSctpTransport)
		throw std::logic_error("Got a DataChannel message without transport");

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
			channel->onOpen(std::bind(&PeerConnection::triggerDataChannel, this, channel));
			mDataChannels.insert(std::make_pair(message->stream, channel));
		} else {
			// Invalid, close the DataChannel by resetting the stream
			mSctpTransport->reset(message->stream);
			return;
		}
	}

	channel->incoming(message);
}

void PeerConnection::openDataChannels(void) {
	auto it = mDataChannels.begin();
	while (it != mDataChannels.end()) {
		auto channel = it->second.lock();
		if (!channel || channel->isClosed()) {
			it = mDataChannels.erase(it);
			continue;
		}
		channel->open(mSctpTransport);
		++it;
	}
}

void PeerConnection::processLocalDescription(Description description) {
	auto remoteSctpPort = mRemoteDescription ? mRemoteDescription->sctpPort() : nullopt;

	description.setFingerprint(mCertificate->fingerprint());
	description.setSctpPort(remoteSctpPort.value_or(DEFAULT_SCTP_PORT));
	mLocalDescription.emplace(std::move(description));

	if (mLocalDescriptionCallback)
		mLocalDescriptionCallback(*mLocalDescription);
}

void PeerConnection::processLocalCandidate(std::optional<Candidate> candidate) {
	if (!mLocalDescription)
		throw std::logic_error("Got a local candidate without local description");

	mLocalDescription->addCandidate(candidate);

	if (mLocalCandidateCallback)
		mLocalCandidateCallback(candidate);
}

void PeerConnection::triggerDataChannel(std::shared_ptr<DataChannel> dataChannel) {
	if (mDataChannelCallback)
		mDataChannelCallback(dataChannel);
}

} // namespace rtc
