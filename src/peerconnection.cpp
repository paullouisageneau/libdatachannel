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
#include "include.hpp"
#include "processor.hpp"
#include "threadpool.hpp"

#include "dtlstransport.hpp"
#include "icetransport.hpp"
#include "sctptransport.hpp"

#if RTC_ENABLE_MEDIA
#include "dtlssrtptransport.hpp"
#endif

#include <iomanip>
#include <thread>

namespace rtc {

using namespace std::placeholders;

using std::shared_ptr;
using std::weak_ptr;

PeerConnection::PeerConnection() : PeerConnection(Configuration()) {}

PeerConnection::PeerConnection(const Configuration &config)
    : mConfig(config), mCertificate(make_certificate()), mProcessor(std::make_unique<Processor>()),
      mState(State::New), mGatheringState(GatheringState::New),
      mSignalingState(SignalingState::Stable), mNegotiationNeeded(false) {
	PLOG_VERBOSE << "Creating PeerConnection";

	if (config.portRangeEnd && config.portRangeBegin > config.portRangeEnd)
		throw std::invalid_argument("Invalid port range");
}

PeerConnection::~PeerConnection() {
	PLOG_VERBOSE << "Destroying PeerConnection";
	close();
	mProcessor->join();
}

void PeerConnection::close() {
	PLOG_VERBOSE << "Closing PeerConnection";

	mNegotiationNeeded = false;

	// Close data channels asynchronously
	mProcessor->enqueue(std::bind(&PeerConnection::closeDataChannels, this));

	closeTransports();
}

const Configuration *PeerConnection::config() const { return &mConfig; }

PeerConnection::State PeerConnection::state() const { return mState; }

PeerConnection::GatheringState PeerConnection::gatheringState() const { return mGatheringState; }

PeerConnection::SignalingState PeerConnection::signalingState() const { return mSignalingState; }

std::optional<Description> PeerConnection::localDescription() const {
	std::lock_guard lock(mLocalDescriptionMutex);
	return mLocalDescription;
}

std::optional<Description> PeerConnection::remoteDescription() const {
	std::lock_guard lock(mRemoteDescriptionMutex);
	return mRemoteDescription;
}

bool PeerConnection::hasLocalDescription() const {
	std::lock_guard lock(mLocalDescriptionMutex);
	return bool(mLocalDescription);
}

bool PeerConnection::hasRemoteDescription() const {
	std::lock_guard lock(mRemoteDescriptionMutex);
	return bool(mRemoteDescription);
}

bool PeerConnection::hasMedia() const {
	auto local = localDescription();
	return local && local->hasAudioOrVideo();
}

void PeerConnection::setLocalDescription(Description::Type type) {
	PLOG_VERBOSE << "Setting local description, type=" << Description::typeToString(type);


	SignalingState signalingState = mSignalingState.load();
	if (type == Description::Type::Rollback) {
		if (signalingState == SignalingState::HaveLocalOffer ||
		    signalingState == SignalingState::HaveLocalPranswer) {
			PLOG_DEBUG << "Rolling back pending local description";

			std::unique_lock lock(mLocalDescriptionMutex);
			if (mCurrentLocalDescription) {
				std::vector<Candidate> existingCandidates;
				if (mLocalDescription)
					existingCandidates = mLocalDescription->extractCandidates();

				mLocalDescription.emplace(std::move(*mCurrentLocalDescription));
				mLocalDescription->addCandidates(std::move(existingCandidates));
				mCurrentLocalDescription.reset();
			}
			lock.unlock();

			changeSignalingState(SignalingState::Stable);
		}
		return;
	}

	// Guess the description type if unspecified
	if (type == Description::Type::Unspec) {
		if (mSignalingState == SignalingState::HaveRemoteOffer)
			type = Description::Type::Answer;
		else
			type = Description::Type::Offer;
	}

	// Only a local offer resets the negotiation needed flag
	if (type == Description::Type::Offer && !mNegotiationNeeded.exchange(false)) {
		PLOG_DEBUG << "No negotiation needed";
		return;
	}

	// Get the new signaling state
	SignalingState newSignalingState;
	switch (signalingState) {
	case SignalingState::Stable:
		if (type != Description::Type::Offer) {
			std::ostringstream oss;
			oss << "Unexpected local desciption type " << type << " in signaling state "
			    << signalingState;
			throw std::logic_error(oss.str());
		}
		newSignalingState = SignalingState::HaveLocalOffer;
		break;

	case SignalingState::HaveRemoteOffer:
	case SignalingState::HaveLocalPranswer:
		if (type != Description::Type::Answer && type != Description::Type::Pranswer) {
			std::ostringstream oss;
			oss << "Unexpected local description type " << type
			    << " description in signaling state " << signalingState;
			throw std::logic_error(oss.str());
		}
		newSignalingState = SignalingState::Stable;
		break;

	default: {
		std::ostringstream oss;
		oss << "Unexpected local description in signaling state " << signalingState << ", ignoring";
		LOG_WARNING << oss.str();
		return;
	}
	}

	auto iceTransport = std::atomic_load(&mIceTransport);
	if (!iceTransport) {
		// RFC 5763: The endpoint that is the offerer MUST use the setup attribute value of
		// setup:actpass.
		// See https://tools.ietf.org/html/rfc5763#section-5
		iceTransport = initIceTransport(Description::Role::ActPass);
	}

	Description localDescription = iceTransport->getLocalDescription(type);
	processLocalDescription(std::move(localDescription));

	changeSignalingState(newSignalingState);

	if (mGatheringState == GatheringState::New)
		iceTransport->gatherLocalCandidates();
}

void PeerConnection::setRemoteDescription(Description description) {
	PLOG_VERBOSE << "Setting remote description: " << string(description);

	if (description.type() == Description::Type::Rollback) {
		// This is mostly useless because we accept any offer
		PLOG_VERBOSE << "Rolling back pending remote description";
		changeSignalingState(SignalingState::Stable);
		return;
	}

	validateRemoteDescription(description);

	// Get the new signaling state
	SignalingState signalingState = mSignalingState.load();
	SignalingState newSignalingState;
	switch (signalingState) {
	case SignalingState::Stable:
		description.hintType(Description::Type::Offer);
		if (description.type() != Description::Type::Offer) {
			std::ostringstream oss;
			oss << "Unexpected remote " << description.type() << " description in signaling state "
			    << signalingState;
			throw std::logic_error(oss.str());
		}
		newSignalingState = SignalingState::HaveRemoteOffer;
		break;

	case SignalingState::HaveLocalOffer:
		description.hintType(Description::Type::Answer);
		if (description.type() == Description::Type::Offer) {
			// The ICE agent will automatically initiate a rollback when a peer that had previously
			// created an offer receives an offer from the remote peer
			setLocalDescription(Description::Type::Rollback);
			newSignalingState = SignalingState::HaveRemoteOffer;
			break;
		}
		if (description.type() != Description::Type::Answer &&
		    description.type() != Description::Type::Pranswer) {
			std::ostringstream oss;
			oss << "Unexpected remote " << description.type() << " description in signaling state "
			    << signalingState;
			throw std::logic_error(oss.str());
		}
		newSignalingState = SignalingState::Stable;
		break;

	case SignalingState::HaveRemotePranswer:
		description.hintType(Description::Type::Answer);
		if (description.type() != Description::Type::Answer &&
		    description.type() != Description::Type::Pranswer) {
			std::ostringstream oss;
			oss << "Unexpected remote " << description.type() << " description in signaling state "
			    << signalingState;
			throw std::logic_error(oss.str());
		}
		newSignalingState = SignalingState::Stable;
		break;

	default: {
		std::ostringstream oss;
		oss << "Unexpected remote description in signaling state " << signalingState;
		throw std::logic_error(oss.str());
	}
	}

	// Candidates will be added at the end, extract them for now
	auto remoteCandidates = description.extractCandidates();
	auto type = description.type();

	auto iceTransport = std::atomic_load(&mIceTransport);
	if (!iceTransport)
		iceTransport = initIceTransport(Description::Role::ActPass);

	iceTransport->setRemoteDescription(description);
	processRemoteDescription(std::move(description));

	changeSignalingState(newSignalingState);

	if (type == Description::Type::Offer) {
		// This is an offer, we need to answer
		setLocalDescription(Description::Type::Answer);
	} else {
		// This is an answer
		auto sctpTransport = std::atomic_load(&mSctpTransport);
		if (!sctpTransport && iceTransport->role() == Description::Role::Active) {
			// Since we assumed passive role during DataChannel creation, we need to shift the
			// stream numbers by one to shift them from odd to even.
			std::unique_lock lock(mDataChannelsMutex); // we are going to swap the container
			decltype(mDataChannels) newDataChannels;
			auto it = mDataChannels.begin();
			while (it != mDataChannels.end()) {
				auto channel = it->second.lock();
				if (channel->stream() % 2 == 1)
					channel->mStream -= 1;
				newDataChannels.emplace(channel->stream(), channel);
				++it;
			}
			std::swap(mDataChannels, newDataChannels);
		}
	}

	for (const auto &candidate : remoteCandidates)
		addRemoteCandidate(candidate);
}

void PeerConnection::addRemoteCandidate(Candidate candidate) {
	PLOG_VERBOSE << "Adding remote candidate: " << string(candidate);
	processRemoteCandidate(std::move(candidate));
}

std::optional<string> PeerConnection::localAddress() const {
	auto iceTransport = std::atomic_load(&mIceTransport);
	return iceTransport ? iceTransport->getLocalAddress() : nullopt;
}

std::optional<string> PeerConnection::remoteAddress() const {
	auto iceTransport = std::atomic_load(&mIceTransport);
	return iceTransport ? iceTransport->getRemoteAddress() : nullopt;
}

shared_ptr<DataChannel> PeerConnection::addDataChannel(string label, string protocol,
                                                       Reliability reliability) {
	// RFC 5763: The answerer MUST use either a setup attribute value of setup:active or
	// setup:passive. [...] Thus, setup:active is RECOMMENDED.
	// See https://tools.ietf.org/html/rfc5763#section-5
	// Therefore, we assume passive role when we are the offerer.
	auto iceTransport = std::atomic_load(&mIceTransport);
	auto role = iceTransport ? iceTransport->role() : Description::Role::Passive;

	auto channel =
	    emplaceDataChannel(role, std::move(label), std::move(protocol), std::move(reliability));

	if (auto transport = std::atomic_load(&mSctpTransport))
		if (transport->state() == SctpTransport::State::Connected)
			channel->open(transport);

	// Renegotiation is needed iff the current local description does not have application
	std::lock_guard lock(mLocalDescriptionMutex);
	if (!mLocalDescription || !mLocalDescription->hasApplication())
		mNegotiationNeeded = true;

	return channel;
}

shared_ptr<DataChannel> PeerConnection::createDataChannel(string label, string protocol,
                                                          Reliability reliability) {
	auto channel = addDataChannel(label, protocol, reliability);
	setLocalDescription();
	return channel;
}

void PeerConnection::onDataChannel(
    std::function<void(shared_ptr<DataChannel> dataChannel)> callback) {
	mDataChannelCallback = callback;
}

void PeerConnection::onLocalDescription(std::function<void(Description description)> callback) {
	mLocalDescriptionCallback = callback;
}

void PeerConnection::onLocalCandidate(std::function<void(Candidate candidate)> callback) {
	mLocalCandidateCallback = callback;
}

void PeerConnection::onStateChange(std::function<void(State state)> callback) {
	mStateChangeCallback = callback;
}

void PeerConnection::onGatheringStateChange(std::function<void(GatheringState state)> callback) {
	mGatheringStateChangeCallback = callback;
}

void PeerConnection::onSignalingStateChange(std::function<void(SignalingState state)> callback) {
	mSignalingStateChangeCallback = callback;
}

std::shared_ptr<Track> PeerConnection::addTrack(Description::Media description) {
#if !RTC_ENABLE_MEDIA
	if (mTracks.empty()) {
		PLOG_WARNING << "Tracks will be inative (not compiled with SRTP support)";
	}
#endif

	std::shared_ptr<Track> track;
	if (auto it = mTracks.find(description.mid()); it != mTracks.end())
		if (track = it->second.lock(); track)
			track->setDescription(std::move(description));

	if (!track) {
		track = std::make_shared<Track>(std::move(description));
		mTracks.emplace(std::make_pair(track->mid(), track));
		mTrackLines.emplace_back(track);
	}

	// Renegotiation is needed for the new or updated track
	mNegotiationNeeded = true;

	return track;
}

void PeerConnection::onTrack(std::function<void(std::shared_ptr<Track>)> callback) {
	mTrackCallback = callback;
}

shared_ptr<IceTransport> PeerConnection::initIceTransport(Description::Role role) {
	PLOG_VERBOSE << "Starting ICE transport";
	try {
		if (auto transport = std::atomic_load(&mIceTransport))
			return transport;

		auto transport = std::make_shared<IceTransport>(
		    mConfig, role, weak_bind(&PeerConnection::processLocalCandidate, this, _1),
		    [this, weak_this = weak_from_this()](IceTransport::State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
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
		    [this, weak_this = weak_from_this()](IceTransport::GatheringState state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
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
		if (mState == State::Closed) {
			mIceTransport.reset();
			throw std::runtime_error("Connection is closed");
		}
		transport->start();
		return transport;

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		changeState(State::Failed);
		throw std::runtime_error("ICE transport initialization failed");
	}
}

shared_ptr<DtlsTransport> PeerConnection::initDtlsTransport() {
	PLOG_VERBOSE << "Starting DTLS transport";
	try {
		if (auto transport = std::atomic_load(&mDtlsTransport))
			return transport;

		auto certificate = mCertificate.get();
		auto lower = std::atomic_load(&mIceTransport);
		auto verifierCallback = weak_bind(&PeerConnection::checkFingerprint, this, _1);
		auto stateChangeCallback = [this,
		                            weak_this = weak_from_this()](DtlsTransport::State state) {
			auto shared_this = weak_this.lock();
			if (!shared_this)
				return;

			switch (state) {
			case DtlsTransport::State::Connected:
				if (auto remote = remoteDescription(); remote && remote->hasApplication())
					initSctpTransport();
				else
					changeState(State::Connected);

				mProcessor->enqueue(std::bind(&PeerConnection::openTracks, this));
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
		};

		shared_ptr<DtlsTransport> transport;
		if (hasMedia()) {
#if RTC_ENABLE_MEDIA
			PLOG_INFO << "This connection requires media support";

			// DTLS-SRTP
			transport = std::make_shared<DtlsSrtpTransport>(
			    lower, certificate, verifierCallback,
			    std::bind(&PeerConnection::forwardMedia, this, _1), stateChangeCallback);
#else
			PLOG_WARNING << "Ignoring media support (not compiled with SRTP support)";
#endif
		}

		if (!transport) {
			// DTLS only
			transport = std::make_shared<DtlsTransport>(lower, certificate, verifierCallback,
			                                            stateChangeCallback);
		}

		std::atomic_store(&mDtlsTransport, transport);
		if (mState == State::Closed) {
			mDtlsTransport.reset();
			throw std::runtime_error("Connection is closed");
		}
		transport->start();
		return transport;

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		changeState(State::Failed);
		throw std::runtime_error("DTLS transport initialization failed");
	}
}

shared_ptr<SctpTransport> PeerConnection::initSctpTransport() {
	PLOG_VERBOSE << "Starting SCTP transport";
	try {
		if (auto transport = std::atomic_load(&mSctpTransport))
			return transport;

		auto remote = remoteDescription();
		if (!remote || !remote->application())
			throw std::logic_error("Starting SCTP transport without application description");

		uint16_t sctpPort = remote->application()->sctpPort().value_or(DEFAULT_SCTP_PORT);
		auto lower = std::atomic_load(&mDtlsTransport);
		auto transport = std::make_shared<SctpTransport>(
		    lower, sctpPort, weak_bind(&PeerConnection::forwardMessage, this, _1),
		    weak_bind(&PeerConnection::forwardBufferedAmount, this, _1, _2),
		    [this, weak_this = weak_from_this()](SctpTransport::State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (state) {
			    case SctpTransport::State::Connected:
				    changeState(State::Connected);
				    mProcessor->enqueue(std::bind(&PeerConnection::openDataChannels, this));
				    break;
			    case SctpTransport::State::Failed:
				    LOG_WARNING << "SCTP transport failed";
				    changeState(State::Failed);
				    mProcessor->enqueue(std::bind(&PeerConnection::remoteCloseDataChannels, this));
				    break;
			    case SctpTransport::State::Disconnected:
				    changeState(State::Disconnected);
				    mProcessor->enqueue(std::bind(&PeerConnection::remoteCloseDataChannels, this));
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });

		std::atomic_store(&mSctpTransport, transport);
		if (mState == State::Closed) {
			mSctpTransport.reset();
			throw std::runtime_error("Connection is closed");
		}
		transport->start();
		return transport;

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		changeState(State::Failed);
		throw std::runtime_error("SCTP transport initialization failed");
	}
}

void PeerConnection::closeTransports() {
	PLOG_VERBOSE << "Closing transports";

	// Change state to sink state Closed
	if (!changeState(State::Closed))
		return; // already closed

	// Reset callbacks now that state is changed
	resetCallbacks();

	// Initiate transport stop on the processor after closing the data channels
	mProcessor->enqueue([this]() {
		// Pass the pointers to a thread
		auto sctp = std::atomic_exchange(&mSctpTransport, decltype(mSctpTransport)(nullptr));
		auto dtls = std::atomic_exchange(&mDtlsTransport, decltype(mDtlsTransport)(nullptr));
		auto ice = std::atomic_exchange(&mIceTransport, decltype(mIceTransport)(nullptr));
		ThreadPool::Instance().enqueue([sctp, dtls, ice]() mutable {
			if (sctp)
				sctp->stop();
			if (dtls)
				dtls->stop();
			if (ice)
				ice->stop();

			sctp.reset();
			dtls.reset();
			ice.reset();
		});
	});
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

	auto channel = findDataChannel(uint16_t(message->stream));
	if (!channel) {
		auto iceTransport = std::atomic_load(&mIceTransport);
		auto sctpTransport = std::atomic_load(&mSctpTransport);
		if (!iceTransport || !sctpTransport)
			return;

		const byte dataChannelOpenMessage{0x03};
		unsigned int remoteParity = (iceTransport->role() == Description::Role::Active) ? 1 : 0;
		if (message->type == Message::Control && *message->data() == dataChannelOpenMessage &&
		    message->stream % 2 == remoteParity) {

			channel =
			    std::make_shared<DataChannel>(shared_from_this(), sctpTransport, message->stream);
			channel->onOpen(weak_bind(&PeerConnection::triggerDataChannel, this,
			                          weak_ptr<DataChannel>{channel}));
			mDataChannels.insert(std::make_pair(message->stream, channel));
		} else {
			// Invalid, close the DataChannel
			sctpTransport->closeStream(message->stream);
			return;
		}
	}

	channel->incoming(message);
}

void PeerConnection::forwardMedia(message_ptr message) {
	if (!message)
		return;


	// Browsers like to compound their packets with a random SSRC.
	// we have to do this monstrosity to distribute the report blocks
//    std::optional<string> mid;
    std::optional<unsigned int> mediaLine;
    if (message->type == Message::Control) {
        unsigned int offset = 0;
        std::vector<SSRC> ssrcsFound;
        bool hasFound = false;

        while ((sizeof(rtc::RTCP_HEADER) + offset) < message->size()) {
            auto header = (rtc::RTCP_HEADER *) (message->data() + offset);
            if (header->lengthInBytes() > message->size() - offset) {
                PLOG_WARNING << "Packet was truncated";
                break;
            } else {
                if (header->payloadType() == 205 || header->payloadType() == 206) {
                    auto rtcpfb = (RTCP_FB_HEADER *) header;
                    auto ssrc = rtcpfb->getPacketSenderSSRC();
                    if (std::find(ssrcsFound.begin(), ssrcsFound.end(), ssrc) != ssrcsFound.end()) {
                        mediaLine = getMLineFromSSRC(ssrc);
                        if (mediaLine.has_value()) {
                            hasFound = true;
                            std::shared_lock lock(mTracksMutex); // read-only
                            if (auto track = mTrackLines[*mediaLine].lock()) {
                                track->incoming(message);
                            }
                            ssrcsFound.emplace_back(ssrc);
                        }
                    }

                    ssrc = rtcpfb->getMediaSourceSSRC();
                    if (std::find(ssrcsFound.begin(), ssrcsFound.end(), ssrc) != ssrcsFound.end()) {
                        mediaLine = getMLineFromSSRC(ssrc);
                        if (mediaLine.has_value()) {
                            hasFound = true;
                            std::shared_lock lock(mTracksMutex); // read-only
                            if (auto track = mTrackLines[*mediaLine].lock()) {
                                track->incoming(message);
                            }
                            ssrcsFound.emplace_back(ssrc);
                        }
                    }
                }else if (header->payloadType() == 200 || header->payloadType() == 201) {
                    auto rtcpsr = (RTCP_SR*) header;
                    auto ssrc = rtcpsr->senderSSRC();
                    if (std::find(ssrcsFound.begin(), ssrcsFound.end(), ssrc) != ssrcsFound.end()) {
                        mediaLine = getMLineFromSSRC(ssrc);
                        if (mediaLine.has_value()) {
                            hasFound = true;
                            std::shared_lock lock(mTracksMutex); // read-only
                            if (auto track = mTrackLines[*mediaLine].lock()) {
                                track->incoming(message);
                            }
                            ssrcsFound.emplace_back(ssrc);
                        }
                    }
                    for (int i = 0; i < rtcpsr->header.reportCount(); i++) {
                        auto block = rtcpsr->getReportBlock(i);
                        ssrc = block->getSSRC();
                        if (std::find(ssrcsFound.begin(), ssrcsFound.end(), ssrc) != ssrcsFound.end()) {
                            mediaLine = getMLineFromSSRC(ssrc);
                            if (mediaLine.has_value()) {
                                hasFound = true;
                                std::shared_lock lock(mTracksMutex); // read-only
                                if (auto track = mTrackLines[*mediaLine].lock()) {
                                    track->incoming(message);
                                }
                                ssrcsFound.emplace_back(ssrc);
                            }
                        }
                    }
                }else {
		    // This warning is commonly thrown with SDES PT=202
                    // PLOG_WARNING << "Unknown packet type: " << (int) header->payloadType();
                }
            }
            offset += header->lengthInBytes();
        }

        if (hasFound)
            return;
    }

    unsigned int ssrc = message->stream;
    mediaLine = getMLineFromSSRC(ssrc);

	if (!mediaLine) {
	    /* TODO
	     *   So the problem is that when stop sending streams, we stop getting report blocks for those streams
	     *   Therefore when we get compound RTCP packets, they are empty, and we can't forward them.
	     *   Therefore, it is expected that we don't know where to forward packets.
	     *   Is this ideal? No! Do I know how to fix it? No!
	     */
		//PLOG_WARNING << "Track not found for SSRC " << ssrc << ", dropping";
		return;
	}

	std::shared_lock lock(mTracksMutex); // read-only
    if (auto track = mTrackLines[*mediaLine].lock()) {
        track->incoming(message);
    }
}

std::optional<unsigned int> PeerConnection::getMLineFromSSRC(SSRC ssrc) {
    if (auto it = mMLineFromSssrc.find(ssrc); it != mMLineFromSssrc.end()) {
        return it->second;
    }else {
        {
            std::lock_guard lock(mRemoteDescriptionMutex);
            if (!mRemoteDescription)
                return nullopt;
            for (unsigned int i = 0; i < mRemoteDescription->mediaCount(); ++i) {
                if (auto found = std::visit(
                        rtc::overloaded{[&](Description::Application *) -> bool {
                            return false;
                        },
                                        [&](Description::Media *media) -> bool {
                                            return media->hasSSRC(ssrc);
                                        }},
                        mRemoteDescription->media(i))) {

                    mMLineFromSssrc.emplace(ssrc, i);
                    return i;
                }
            }
        }
        {
            std::lock_guard lock(mLocalDescriptionMutex);
            if (!mLocalDescription)
                return nullopt;
            for (unsigned int i = 0; i < mLocalDescription->mediaCount(); ++i) {
                if (auto found = std::visit(
                        rtc::overloaded{[&](Description::Application *) -> bool {
                            return false;
                        },
                                        [&](Description::Media *media) -> bool {
                                            return media->hasSSRC(ssrc);
                                        }},
                        mLocalDescription->media(i))) {

                    mMLineFromSssrc.emplace(ssrc, i);
                    return i;
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> PeerConnection::getMidFromSSRC(SSRC ssrc) {
    if (auto it = mMidFromSssrc.find(ssrc); it != mMidFromSssrc.end()) {
        return it->second;
    } else {
        {
            std::lock_guard lock(mRemoteDescriptionMutex);
            if (!mRemoteDescription)
                return nullopt;
            for (unsigned int i = 0; i < mRemoteDescription->mediaCount(); ++i) {
                if (auto found = std::visit(
                        rtc::overloaded{[&](Description::Application *) -> std::optional<string> {
                            return std::nullopt;
                        },
                                        [&](Description::Media *media) -> std::optional<string> {
                                            return media->hasSSRC(ssrc)
                                                   ? std::make_optional(media->mid())
                                                   : nullopt;
                                        }},
                        mRemoteDescription->media(i))) {

                    mMidFromSssrc.emplace(ssrc, *found);
                    return *found;
                }
            }
        }
        {
            std::lock_guard lock(mLocalDescriptionMutex);
            if (!mLocalDescription)
                return nullopt;
            for (unsigned int i = 0; i < mLocalDescription->mediaCount(); ++i) {
                if (auto found = std::visit(
                        rtc::overloaded{[&](Description::Application *) -> std::optional<string> {
                            return std::nullopt;
                        },
                                        [&](Description::Media *media) -> std::optional<string> {
                                            return media->hasSSRC(ssrc)
                                                   ? std::make_optional(media->mid())
                                                   : nullopt;
                                        }},
                        mLocalDescription->media(i))) {

                    mMidFromSssrc.emplace(ssrc, *found);
                    return *found;
                }
            }
        }
    }
    return nullopt;
}

void PeerConnection::forwardBufferedAmount(uint16_t stream, size_t amount) {
	if (auto channel = findDataChannel(stream))
		channel->triggerBufferedAmount(amount);
}

shared_ptr<DataChannel> PeerConnection::emplaceDataChannel(Description::Role role, string label,
                                                           string protocol,
                                                           Reliability reliability) {
	// The active side must use streams with even identifiers, whereas the passive side must use
	// streams with odd identifiers.
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09#section-6
	std::unique_lock lock(mDataChannelsMutex); // we are going to emplace
	unsigned int stream = (role == Description::Role::Active) ? 0 : 1;
	while (mDataChannels.find(stream) != mDataChannels.end()) {
		stream += 2;
		if (stream >= 65535)
			throw std::runtime_error("Too many DataChannels");
	}
	auto channel = std::make_shared<DataChannel>(shared_from_this(), stream, std::move(label),
	                                             std::move(protocol), std::move(reliability));
	mDataChannels.emplace(std::make_pair(stream, channel));
	return channel;
}

shared_ptr<DataChannel> PeerConnection::findDataChannel(uint16_t stream) {
	std::shared_lock lock(mDataChannelsMutex); // read-only
	if (auto it = mDataChannels.find(stream); it != mDataChannels.end())
		if (auto channel = it->second.lock())
			return channel;

	return nullptr;
}

void PeerConnection::iterateDataChannels(
    std::function<void(shared_ptr<DataChannel> channel)> func) {
	// Iterate
	{
		std::shared_lock lock(mDataChannelsMutex); // read-only
		auto it = mDataChannels.begin();
		while (it != mDataChannels.end()) {
			auto channel = it->second.lock();
			if (channel && !channel->isClosed())
				func(channel);

			++it;
		}
	}

	// Cleanup
	{
		std::unique_lock lock(mDataChannelsMutex); // we are going to erase
		auto it = mDataChannels.begin();
		while (it != mDataChannels.end()) {
			if (!it->second.lock()) {
				it = mDataChannels.erase(it);
				continue;
			}

			++it;
		}
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

void PeerConnection::incomingTrack(Description::Media description) {
	std::unique_lock lock(mTracksMutex); // we are going to emplace
#if !RTC_ENABLE_MEDIA
	if (mTracks.empty()) {
		PLOG_WARNING << "Tracks will be inative (not compiled with SRTP support)";
	}
#endif
	if (mTracks.find(description.mid()) == mTracks.end()) {
		auto track = std::make_shared<Track>(std::move(description));
		mTracks.emplace(std::make_pair(track->mid(), track));
		triggerTrack(std::move(track));
	}
}

void PeerConnection::openTracks() {
#if RTC_ENABLE_MEDIA
	if (auto transport = std::atomic_load(&mDtlsTransport)) {
		auto srtpTransport = std::reinterpret_pointer_cast<DtlsSrtpTransport>(transport);
		std::shared_lock lock(mTracksMutex); // read-only
		for (auto it = mTracks.begin(); it != mTracks.end(); ++it)
			if (auto track = it->second.lock())
				if (!track->isOpen())
					track->open(srtpTransport);
	}
#endif
}

void PeerConnection::validateRemoteDescription(const Description &description) {
	if (!description.iceUfrag())
		throw std::invalid_argument("Remote description has no ICE user fragment");

	if (!description.icePwd())
		throw std::invalid_argument("Remote description has no ICE password");

	if (!description.fingerprint())
		throw std::invalid_argument("Remote description has no fingerprint");

	if (description.mediaCount() == 0)
		throw std::invalid_argument("Remote description has no media line");

	int activeMediaCount = 0;
	for (size_t i = 0; i < description.mediaCount(); ++i)
		std::visit(rtc::overloaded{[&](const Description::Application *) { ++activeMediaCount; },
		                           [&](const Description::Media *media) {
			                           if (media->direction() != Description::Direction::Inactive)
				                           ++activeMediaCount;
		                           }},
		           description.media(i));

	if (activeMediaCount == 0)
		throw std::invalid_argument("Remote description has no active media");

	if (auto local = localDescription(); local && local->iceUfrag() && local->icePwd())
		if (*description.iceUfrag() == *local->iceUfrag() &&
		    *description.icePwd() == *local->icePwd())
			throw std::logic_error("Got the local description as remote description");

	PLOG_VERBOSE << "Remote description looks valid";
}

void PeerConnection::processLocalDescription(Description description) {

	if (auto remote = remoteDescription()) {
		// Reciprocate remote description
		for (size_t i = 0; i < remote->mediaCount(); ++i)
			std::visit( // reciprocate each media
			    rtc::overloaded{
			        [&](Description::Application *remoteApp) {
				        std::shared_lock lock(mDataChannelsMutex);
				        if (!mDataChannels.empty()) {
					        // Prefer local description
					        Description::Application app(remoteApp->mid());
					        app.setSctpPort(DEFAULT_SCTP_PORT);
					        app.setMaxMessageSize(LOCAL_MAX_MESSAGE_SIZE);

					        PLOG_DEBUG << "Adding application to local description, mid=\""
					                   << app.mid() << "\"";

					        description.addMedia(std::move(app));
					        return;
				        }

				        auto reciprocated = remoteApp->reciprocate();
				        reciprocated.hintSctpPort(DEFAULT_SCTP_PORT);
				        reciprocated.setMaxMessageSize(LOCAL_MAX_MESSAGE_SIZE);

				        PLOG_DEBUG << "Reciprocating application in local description, mid=\""
				                   << reciprocated.mid() << "\"";

				        description.addMedia(std::move(reciprocated));
			        },
			        [&](Description::Media *remoteMedia) {
				        std::shared_lock lock(mTracksMutex);
				        if (auto it = mTracks.find(remoteMedia->mid()); it != mTracks.end()) {
					        // Prefer local description
					        if (auto track = it->second.lock()) {
						        auto media = track->description();
#if !RTC_ENABLE_MEDIA
						        // No media support, mark as inactive
						        media.setDirection(Description::Direction::Inactive);
#endif
						        PLOG_DEBUG
						            << "Adding media to local description, mid=\"" << media.mid()
						            << "\", active=" << std::boolalpha
						            << (media.direction() != Description::Direction::Inactive);

						        description.addMedia(std::move(media));
					        } else {
						        auto reciprocated = remoteMedia->reciprocate();
						        reciprocated.setDirection(Description::Direction::Inactive);

						        PLOG_DEBUG << "Adding inactive media to local description, mid=\""
						                   << reciprocated.mid() << "\"";

						        description.addMedia(std::move(reciprocated));
					        }
					        return;
				        }

				        auto reciprocated = remoteMedia->reciprocate();
#if !RTC_ENABLE_MEDIA
                                    // No media support, mark as inactive
                                    reciprocated.setDirection(Description::Direction::Inactive);
#endif
                                    incomingTrack(reciprocated);

                                    PLOG_DEBUG
                                                << "Reciprocating media in local description, mid=\""
                                                << reciprocated.mid() << "\", active=" << std::boolalpha
                                                << (reciprocated.direction() != Description::Direction::Inactive);

                                    description.addMedia(std::move(reciprocated));
                                },
                        },
                        remote->media(i));
        }

	if (description.type() == Description::Type::Offer) {
		// This is an offer, add locally created data channels and tracks
		// Add application for data channels
		if (!description.hasApplication()) {
			std::shared_lock lock(mDataChannelsMutex);
			if (!mDataChannels.empty()) {
				Description::Application app("data");
				app.setSctpPort(DEFAULT_SCTP_PORT);
				app.setMaxMessageSize(LOCAL_MAX_MESSAGE_SIZE);

				PLOG_DEBUG << "Adding application to local description, mid=\"" << app.mid()
				           << "\"";

				description.addMedia(std::move(app));
			}
		}

		// Add media for local tracks

		std::shared_lock lock(mTracksMutex);
        for (auto it = mTrackLines.begin(); it != mTrackLines.end(); ++it) {
            if (auto track = it->lock()) {
                if (description.hasMid(track->mid()))
                    continue;

                auto media = track->description();
#if !RTC_ENABLE_MEDIA
                // No media support, mark as inactive
                media.setDirection(Description::Direction::Inactive);
#endif
                PLOG_DEBUG << "Adding media to local description, mid=\"" << media.mid()
                           << "\", active=" << std::boolalpha
                           << (media.direction() != Description::Direction::Inactive);

                description.addMedia(std::move(media));
            }
        }
	}

	// Set local fingerprint (wait for certificate if necessary)
	description.setFingerprint(mCertificate.get()->fingerprint());

	{
		// Set as local description
		std::lock_guard lock(mLocalDescriptionMutex);

		std::vector<Candidate> existingCandidates;
		if (mLocalDescription) {
			existingCandidates = mLocalDescription->extractCandidates();
			mCurrentLocalDescription.emplace(std::move(*mLocalDescription));
		}

		mLocalDescription.emplace(std::move(description));
		mLocalDescription->addCandidates(std::move(existingCandidates));
	}

	mProcessor->enqueue([this, description = *mLocalDescription]() {
		PLOG_VERBOSE << "Issuing local description: " << description;
		mLocalDescriptionCallback(std::move(description));
	});

	// Reciprocated tracks might need to be open
	if (auto dtlsTransport = std::atomic_load(&mDtlsTransport);
	    dtlsTransport && dtlsTransport->state() == Transport::State::Connected)
		mProcessor->enqueue(std::bind(&PeerConnection::openTracks, this));
}

void PeerConnection::processLocalCandidate(Candidate candidate) {
	std::lock_guard lock(mLocalDescriptionMutex);
	if (!mLocalDescription)
		throw std::logic_error("Got a local candidate without local description");

	candidate.resolve(Candidate::ResolveMode::Simple); // for proper SDP generation later
	mLocalDescription->addCandidate(candidate);

	mProcessor->enqueue([this, candidate = std::move(candidate)]() {
		PLOG_VERBOSE << "Issuing local candidate: " << candidate;
		mLocalCandidateCallback(std::move(candidate));
	});
}

void PeerConnection::processRemoteDescription(Description description) {
	{
		// Set as remote description
		std::lock_guard lock(mRemoteDescriptionMutex);

		std::vector<Candidate> existingCandidates;
		if (mRemoteDescription)
			existingCandidates = mRemoteDescription->extractCandidates();

		mRemoteDescription.emplace(std::move(description));
		mRemoteDescription->addCandidates(std::move(existingCandidates));
	}

	if (description.hasApplication()) {
		auto dtlsTransport = std::atomic_load(&mDtlsTransport);
		auto sctpTransport = std::atomic_load(&mSctpTransport);
		if (!sctpTransport && dtlsTransport &&
		    dtlsTransport->state() == Transport::State::Connected)
			initSctpTransport();
	}
}

void PeerConnection::processRemoteCandidate(Candidate candidate) {
	auto iceTransport = std::atomic_load(&mIceTransport);
	if (!iceTransport)
		throw std::logic_error("Remote candidate set without remote description");

	if (candidate.resolve(Candidate::ResolveMode::Simple)) {
		iceTransport->addRemoteCandidate(candidate);
	} else {
		// OK, we might need a lookup, do it asynchronously
		// We don't use the thread pool because we have no control on the timeout
		weak_ptr<IceTransport> weakIceTransport{iceTransport};
		std::thread t([weakIceTransport, candidate]() mutable {
			if (candidate.resolve(Candidate::ResolveMode::Lookup))
				if (auto iceTransport = weakIceTransport.lock())
					iceTransport->addRemoteCandidate(candidate);
		});
		t.detach();
	}

	{
		std::lock_guard lock(mRemoteDescriptionMutex);
		if (!mRemoteDescription)
			throw std::logic_error("Got a remote candidate without remote description");

		mRemoteDescription->addCandidate(candidate);
	}
}

void PeerConnection::triggerDataChannel(weak_ptr<DataChannel> weakDataChannel) {
	auto dataChannel = weakDataChannel.lock();
	if (!dataChannel)
		return;

	mProcessor->enqueue(
	    [this, dataChannel = std::move(dataChannel)]() { mDataChannelCallback(dataChannel); });
}

void PeerConnection::triggerTrack(std::shared_ptr<Track> track) {
	mProcessor->enqueue([this, track = std::move(track)]() { mTrackCallback(track); });
}

bool PeerConnection::changeState(State state) {
	State current;
	do {
		current = mState.load();
		if (current == State::Closed)
			return false;
		if (current == state)
			return false;

	} while (!mState.compare_exchange_weak(current, state));

	std::ostringstream s;
	s << state;
	PLOG_INFO << "Changed state to " << s.str();

	if (state == State::Closed)
		// This is the last state change, so we may steal the callback
		mProcessor->enqueue([cb = std::move(mStateChangeCallback)]() { cb(State::Closed); });
	else
		mProcessor->enqueue([this, state]() { mStateChangeCallback(state); });

	return true;
}

bool PeerConnection::changeGatheringState(GatheringState state) {
	if (mGatheringState.exchange(state) == state)
		return false;

	std::ostringstream s;
	s << state;
	PLOG_INFO << "Changed gathering state to " << s.str();
	mProcessor->enqueue([this, state] { mGatheringStateChangeCallback(state); });
	return true;
}

bool PeerConnection::changeSignalingState(SignalingState state) {
	if (mSignalingState.exchange(state) == state)
		return false;

	std::ostringstream s;
	s << state;
	PLOG_INFO << "Changed signaling state to " << s.str();
	mProcessor->enqueue([this, state] { mSignalingStateChangeCallback(state); });
	return true;
}

void PeerConnection::resetCallbacks() {
	// Unregister all callbacks
	mDataChannelCallback = nullptr;
	mLocalDescriptionCallback = nullptr;
	mLocalCandidateCallback = nullptr;
	mStateChangeCallback = nullptr;
	mGatheringStateChangeCallback = nullptr;
}

bool PeerConnection::getSelectedCandidatePair([[maybe_unused]] Candidate *local,
                                              [[maybe_unused]] Candidate *remote) {
	auto iceTransport = std::atomic_load(&mIceTransport);
	return iceTransport ? iceTransport->getSelectedCandidatePair(local, remote) : false;
}

void PeerConnection::clearStats() {
	auto sctpTransport = std::atomic_load(&mSctpTransport);
	if (sctpTransport)
		return sctpTransport->clearStats();
}

size_t PeerConnection::bytesSent() {
	auto sctpTransport = std::atomic_load(&mSctpTransport);
	if (sctpTransport)
		return sctpTransport->bytesSent();
	return 0;
}

size_t PeerConnection::bytesReceived() {
	auto sctpTransport = std::atomic_load(&mSctpTransport);
	if (sctpTransport)
		return sctpTransport->bytesReceived();
	return 0;
}

std::optional<std::chrono::milliseconds> PeerConnection::rtt() {
	auto sctpTransport = std::atomic_load(&mSctpTransport);
	if (sctpTransport)
		return sctpTransport->rtt();
	return std::nullopt;
}

} // namespace rtc

std::ostream &operator<<(std::ostream &out, rtc::PeerConnection::State state) {
	using State = rtc::PeerConnection::State;
	const char *str;
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
	default:
		str = "unknown";
		break;
	}
	return out << str;
}

std::ostream &operator<<(std::ostream &out, rtc::PeerConnection::GatheringState state) {
	using GatheringState = rtc::PeerConnection::GatheringState;
	const char *str;
	switch (state) {
	case GatheringState::New:
		str = "new";
		break;
	case GatheringState::InProgress:
		str = "in-progress";
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

std::ostream &operator<<(std::ostream &out, rtc::PeerConnection::SignalingState state) {
	using SignalingState = rtc::PeerConnection::SignalingState;
	const char *str;
	switch (state) {
	case SignalingState::Stable:
		str = "stable";
		break;
	case SignalingState::HaveLocalOffer:
		str = "have-local-offer";
		break;
	case SignalingState::HaveRemoteOffer:
		str = "have-remote-offer";
		break;
	case SignalingState::HaveLocalPranswer:
		str = "have-local-pranswer";
		break;
	case SignalingState::HaveRemotePranswer:
		str = "have-remote-pranswer";
		break;
	default:
		str = "unknown";
		break;
	}
	return out << str;
}
