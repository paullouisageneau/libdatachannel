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
#include "dtlstransport.hpp"
#include "icetransport.hpp"
#include "sctptransport.hpp"

#include <chrono>
#include <random>

namespace rtc {

using namespace std::placeholders;

using std::function;
using std::shared_ptr;

PeerConnection::PeerConnection(const IceConfiguration &config)
    : mConfig(config), mCertificate(make_certificate("libdatachannel")), mMid("0") {}

PeerConnection::~PeerConnection() {}

const IceConfiguration *PeerConnection::config() const { return &mConfig; }

const Certificate *PeerConnection::certificate() const { return &mCertificate; }

void PeerConnection::setRemoteDescription(const string &description) {
	Description desc(Description::Role::ActPass, description);

	if(auto fingerprint = desc.fingerprint())
		mRemoteFingerprint.emplace(*fingerprint);

	if (!mIceTransport) {
		initIceTransport(Description::Role::ActPass);
		mIceTransport->setRemoteDescription(desc);
		triggerLocalDescription();
		mIceTransport->gatherLocalCandidates();
	} else {
		mIceTransport->setRemoteDescription(desc);
	}
}

void PeerConnection::setRemoteCandidate(const string &candidate) {
	Candidate cand(candidate, mMid);
	if (mIceTransport) {
		mIceTransport->addRemoteCandidate(cand);
	}
}

shared_ptr<DataChannel> PeerConnection::createDataChannel(const string &label,
                                                          const string &protocol,
                                                          const Reliability &reliability) {
	auto seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::default_random_engine generator(seed);
	std::uniform_int_distribution<uint16_t> uniform;
	uint16_t stream = uniform(generator);

	auto channel = std::make_shared<DataChannel>(stream, label, protocol, reliability);
	mDataChannels.insert(std::make_pair(stream, channel));

	if (!mIceTransport) {
		initIceTransport(Description::Role::Active);
		triggerLocalDescription();
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

void PeerConnection::onLocalDescription(std::function<void(const string &description)> callback) {
	mLocalDescriptionCallback = callback;
}

void PeerConnection::onLocalCandidate(
    std::function<void(const std::optional<string> &candidate)> callback) {
	mLocalCandidateCallback = callback;
}

void PeerConnection::initIceTransport(Description::Role role) {
	mIceTransport = std::make_shared<IceTransport>(
	    mConfig, role, std::bind(&PeerConnection::triggerLocalCandidate, this, _1),
	    std::bind(&PeerConnection::initDtlsTransport, this));
}

void PeerConnection::initDtlsTransport() {
	mDtlsTransport = std::make_shared<DtlsTransport>(
	    mIceTransport, mCertificate, std::bind(&PeerConnection::checkFingerprint, this, _1),
	    std::bind(&PeerConnection::initSctpTransport, this));
}

void PeerConnection::initSctpTransport() {
	mSctpTransport = std::make_shared<SctpTransport>(
	    mDtlsTransport, std::bind(&PeerConnection::openDataChannels, this),
	    std::bind(&PeerConnection::forwardMessage, this, _1));
}

bool PeerConnection::checkFingerprint(const std::string &fingerprint) const {
	return mRemoteFingerprint && *mRemoteFingerprint == fingerprint;
}

void PeerConnection::forwardMessage(message_ptr message) {
	shared_ptr<DataChannel> channel;
	if (auto it = mDataChannels.find(message->stream); it != mDataChannels.end()) {
		channel = it->second;
	} else {
		channel = std::make_shared<DataChannel>(message->stream, mSctpTransport);
		channel->onOpen(std::bind(&PeerConnection::triggerDataChannel, this, channel));
		mDataChannels.insert(std::make_pair(message->stream, channel));
	}

	channel->incoming(message);
}

void PeerConnection::openDataChannels(void) {
	for (auto it = mDataChannels.begin(); it != mDataChannels.end(); ++it) {
		it->second->open(mSctpTransport);
	}
}

void PeerConnection::triggerLocalDescription() {
	if (mLocalDescriptionCallback && mIceTransport) {
		Description desc{mIceTransport->getLocalDescription()};
		desc.setFingerprint(mCertificate.fingerprint());
		mLocalDescriptionCallback(string(desc));
	}
}

void PeerConnection::triggerLocalCandidate(const std::optional<Candidate> &candidate) {
	if (mLocalCandidateCallback) {
		mLocalCandidateCallback(candidate ? std::make_optional(string(*candidate)) : nullopt);
	}
}

void PeerConnection::triggerDataChannel(std::shared_ptr<DataChannel> dataChannel) {
	if (mDataChannelCallback)
		mDataChannelCallback(dataChannel);
}

} // namespace rtc

