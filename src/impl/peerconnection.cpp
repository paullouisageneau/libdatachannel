/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 * Copyright (c) 2020 Filip Klembara (in2core)
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
#include "common.hpp"
#include "dtlstransport.hpp"
#include "internals.hpp"
#include "icetransport.hpp"
#include "logcounter.hpp"
#include "peerconnection.hpp"
#include "processor.hpp"
#include "rtp.hpp"
#include "sctptransport.hpp"
#include "threadpool.hpp"

#if RTC_ENABLE_MEDIA
#include "dtlssrtptransport.hpp"
#endif

#include <iomanip>
#include <set>
#include <thread>

using namespace std::placeholders;

namespace rtc::impl {

static LogCounter COUNTER_MEDIA_TRUNCATED(plog::warning,
                                          "Number of RTP packets truncated over past second");
static LogCounter COUNTER_SRTP_DECRYPT_ERROR(plog::warning,
                                             "Number of SRTP decryption errors over past second");
static LogCounter COUNTER_SRTP_ENCRYPT_ERROR(plog::warning,
                                             "Number of SRTP encryption errors over past second");
static LogCounter
    COUNTER_UNKNOWN_PACKET_TYPE(plog::warning,
                                "Number of unknown RTCP packet types over past second");

PeerConnection::PeerConnection(Configuration config_)
    : config(std::move(config_)), mCertificate(make_certificate(config.certificateType)),
      mProcessor(std::make_unique<Processor>()) {
	PLOG_VERBOSE << "Creating PeerConnection";

	if (config.portRangeEnd && config.portRangeBegin > config.portRangeEnd)
		throw std::invalid_argument("Invalid port range");

	if (config.mtu) {
		if (*config.mtu < 576) // Min MTU for IPv4
			throw std::invalid_argument("Invalid MTU value");

		if (*config.mtu > 1500) { // Standard Ethernet
			PLOG_WARNING << "MTU set to " << *config.mtu;
		} else {
			PLOG_VERBOSE << "MTU set to " << *config.mtu;
		}
	}
}

PeerConnection::~PeerConnection() {
	PLOG_VERBOSE << "Destroying PeerConnection";
	mProcessor->join();
}

void PeerConnection::close() {
	PLOG_VERBOSE << "Closing PeerConnection";

	negotiationNeeded = false;

	// Close data channels asynchronously
	mProcessor->enqueue(&PeerConnection::closeDataChannels, this);

	closeTransports();
}

optional<Description> PeerConnection::localDescription() const {
	std::lock_guard lock(mLocalDescriptionMutex);
	return mLocalDescription;
}

optional<Description> PeerConnection::remoteDescription() const {
	std::lock_guard lock(mRemoteDescriptionMutex);
	return mRemoteDescription;
}

size_t PeerConnection::remoteMaxMessageSize() const {
	const size_t localMax = config.maxMessageSize.value_or(DEFAULT_LOCAL_MAX_MESSAGE_SIZE);

	size_t remoteMax = DEFAULT_MAX_MESSAGE_SIZE;
	std::lock_guard lock(mRemoteDescriptionMutex);
	if (mRemoteDescription)
		if (auto *application = mRemoteDescription->application())
			if (auto max = application->maxMessageSize()) {
				// RFC 8841: If the SDP "max-message-size" attribute contains a maximum message
				// size value of zero, it indicates that the SCTP endpoint will handle messages
				// of any size, subject to memory capacity, etc.
				remoteMax = *max > 0 ? *max : std::numeric_limits<size_t>::max();
			}

	return std::min(remoteMax, localMax);
}

shared_ptr<IceTransport> PeerConnection::initIceTransport() {
	try {
		if (auto transport = std::atomic_load(&mIceTransport))
			return transport;

		PLOG_VERBOSE << "Starting ICE transport";

		auto transport = std::make_shared<IceTransport>(
		    config, weak_bind(&PeerConnection::processLocalCandidate, this, _1),
		    [this, weak_this = weak_from_this()](IceTransport::State transportState) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (transportState) {
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
		    [this, weak_this = weak_from_this()](IceTransport::GatheringState gatheringState) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (gatheringState) {
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
		if (state.load() == State::Closed) {
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
	try {
		if (auto transport = std::atomic_load(&mDtlsTransport))
			return transport;

		PLOG_VERBOSE << "Starting DTLS transport";

		auto certificate = mCertificate.get();
		auto lower = std::atomic_load(&mIceTransport);
		auto verifierCallback = weak_bind(&PeerConnection::checkFingerprint, this, _1);
		auto dtlsStateChangeCallback =
		    [this, weak_this = weak_from_this()](DtlsTransport::State transportState) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;

			    switch (transportState) {
			    case DtlsTransport::State::Connected:
				    if (auto remote = remoteDescription(); remote && remote->hasApplication())
					    initSctpTransport();
				    else
					    changeState(State::Connected);

				    mProcessor->enqueue(&PeerConnection::openTracks, this);
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
		if (auto local = localDescription(); local && local->hasAudioOrVideo()) {
#if RTC_ENABLE_MEDIA
			PLOG_INFO << "This connection requires media support";

			// DTLS-SRTP
			transport = std::make_shared<DtlsSrtpTransport>(
			    lower, certificate, config.mtu, verifierCallback,
			    weak_bind(&PeerConnection::forwardMedia, this, _1), dtlsStateChangeCallback);
#else
			PLOG_WARNING << "Ignoring media support (not compiled with media support)";
#endif
		}

		if (!transport) {
			// DTLS only
			transport = std::make_shared<DtlsTransport>(lower, certificate, config.mtu,
			                                            verifierCallback, dtlsStateChangeCallback);
		}

		std::atomic_store(&mDtlsTransport, transport);
		if (state.load() == State::Closed) {
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
	try {
		if (auto transport = std::atomic_load(&mSctpTransport))
			return transport;

		PLOG_VERBOSE << "Starting SCTP transport";

		auto remote = remoteDescription();
		if (!remote || !remote->application())
			throw std::logic_error("Starting SCTP transport without application description");

		// This is the last occasion to ensure the stream numbers are coherent with the role
		shiftDataChannels();

		uint16_t sctpPort = remote->application()->sctpPort().value_or(DEFAULT_SCTP_PORT);
		auto lower = std::atomic_load(&mDtlsTransport);
		auto transport = std::make_shared<SctpTransport>(
		    lower, config, sctpPort, weak_bind(&PeerConnection::forwardMessage, this, _1),
		    weak_bind(&PeerConnection::forwardBufferedAmount, this, _1, _2),
		    [this, weak_this = weak_from_this()](SctpTransport::State transportState) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (transportState) {
			    case SctpTransport::State::Connected:
				    changeState(State::Connected);
				    mProcessor->enqueue(&PeerConnection::openDataChannels, this);
				    break;
			    case SctpTransport::State::Failed:
				    LOG_WARNING << "SCTP transport failed";
				    changeState(State::Failed);
				    mProcessor->enqueue(&PeerConnection::remoteCloseDataChannels, this);
				    break;
			    case SctpTransport::State::Disconnected:
				    changeState(State::Disconnected);
				    mProcessor->enqueue(&PeerConnection::remoteCloseDataChannels, this);
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });

		std::atomic_store(&mSctpTransport, transport);
		if (state.load() == State::Closed) {
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

shared_ptr<IceTransport> PeerConnection::getIceTransport() const {
	return std::atomic_load(&mIceTransport);
}

shared_ptr<DtlsTransport> PeerConnection::getDtlsTransport() const {
	return std::atomic_load(&mDtlsTransport);
}

shared_ptr<SctpTransport> PeerConnection::getSctpTransport() const {
	return std::atomic_load(&mSctpTransport);
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

void PeerConnection::rollbackLocalDescription() {
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
}

bool PeerConnection::checkFingerprint(const std::string &fingerprint) const {
	std::lock_guard lock(mRemoteDescriptionMutex);
	auto expectedFingerprint = mRemoteDescription ? mRemoteDescription->fingerprint() : nullopt;
	if (expectedFingerprint && *expectedFingerprint == fingerprint) {
		PLOG_VERBOSE << "Valid fingerprint \"" << fingerprint << "\"";
		return true;
	}

	PLOG_ERROR << "Invalid fingerprint \"" << fingerprint << "\", expected \""
	           << expectedFingerprint.value_or("[none]") << "\"";
	return false;
}

void PeerConnection::forwardMessage(message_ptr message) {
	if (!message) {
		remoteCloseDataChannels();
		return;
	}

	uint16_t stream = uint16_t(message->stream);
	auto channel = findDataChannel(stream);
	if (!channel) {
		auto iceTransport = getIceTransport();
		auto sctpTransport = getSctpTransport();
		if (!iceTransport || !sctpTransport)
			return;

		const byte dataChannelOpenMessage{0x03};
		uint16_t remoteParity = (iceTransport->role() == Description::Role::Active) ? 1 : 0;
		if (message->type == Message::Control && *message->data() == dataChannelOpenMessage &&
		    stream % 2 == remoteParity) {

			channel =
			    std::make_shared<NegotiatedDataChannel>(weak_from_this(), sctpTransport, stream);
			channel->openCallback = weak_bind(&PeerConnection::triggerDataChannel, this,
			                                  weak_ptr<DataChannel>{channel});

			std::unique_lock lock(mDataChannelsMutex); // we are going to emplace
			mDataChannels.emplace(stream, channel);
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
	if (message->type == Message::Control) {
		std::set<uint32_t> ssrcs;
		size_t offset = 0;
		while ((sizeof(rtc::RTCP_HEADER) + offset) <= message->size()) {
			auto header = reinterpret_cast<rtc::RTCP_HEADER *>(message->data() + offset);
			if (header->lengthInBytes() > message->size() - offset) {
				COUNTER_MEDIA_TRUNCATED++;
				break;
			}
			offset += header->lengthInBytes();
			if (header->payloadType() == 205 || header->payloadType() == 206) {
				auto rtcpfb = reinterpret_cast<RTCP_FB_HEADER *>(header);
				ssrcs.insert(rtcpfb->packetSenderSSRC());
				ssrcs.insert(rtcpfb->mediaSourceSSRC());

			} else if (header->payloadType() == 200 || header->payloadType() == 201) {
				auto rtcpsr = reinterpret_cast<RTCP_SR *>(header);
				ssrcs.insert(rtcpsr->senderSSRC());
				for (int i = 0; i < rtcpsr->header.reportCount(); ++i)
					ssrcs.insert(rtcpsr->getReportBlock(i)->getSSRC());
			} else if (header->payloadType() == 202) {
				auto sdes = reinterpret_cast<RTCP_SDES *>(header);
				if (!sdes->isValid()) {
					PLOG_WARNING << "RTCP SDES packet is invalid";
					continue;
				}
				for (unsigned int i = 0; i < sdes->chunksCount(); i++) {
					auto chunk = sdes->getChunk(i);
					ssrcs.insert(chunk->ssrc());
				}
			} else {
				// PT=207 == Extended Report
				if (header->payloadType() != 207) {
					COUNTER_UNKNOWN_PACKET_TYPE++;
				}
			}
		}

		if (!ssrcs.empty()) {
			for (uint32_t ssrc : ssrcs) {
				if (auto mid = getMidFromSsrc(ssrc)) {
					std::shared_lock lock(mTracksMutex); // read-only
					if (auto it = mTracks.find(*mid); it != mTracks.end())
						if (auto track = it->second.lock())
							track->incoming(message);
				}
			}
			return;
		}
	}

	uint32_t ssrc = uint32_t(message->stream);
	if (auto mid = getMidFromSsrc(ssrc)) {
		std::shared_lock lock(mTracksMutex); // read-only
		if (auto it = mTracks.find(*mid); it != mTracks.end())
			if (auto track = it->second.lock())
				track->incoming(message);
	} else {
		/*
		 * TODO: So the problem is that when stop sending streams, we stop getting report blocks for
		 * those streams Therefore when we get compound RTCP packets, they are empty, and we can't
		 * forward them. Therefore, it is expected that we don't know where to forward packets. Is
		 * this ideal? No! Do I know how to fix it? No!
		 */
		// PLOG_WARNING << "Track not found for SSRC " << ssrc << ", dropping";
		return;
	}
}

optional<std::string> PeerConnection::getMidFromSsrc(uint32_t ssrc) {
	if (auto it = mMidFromSsrc.find(ssrc); it != mMidFromSsrc.end())
		return it->second;

	{
		std::lock_guard lock(mRemoteDescriptionMutex);
		if (!mRemoteDescription)
			return nullopt;
		for (unsigned int i = 0; i < mRemoteDescription->mediaCount(); ++i) {
			if (auto found =
			        std::visit(rtc::overloaded{[&](Description::Application *) -> optional<string> {
				                                   return std::nullopt;
			                                   },
			                                   [&](Description::Media *media) -> optional<string> {
				                                   return media->hasSSRC(ssrc)
				                                              ? std::make_optional(media->mid())
				                                              : nullopt;
			                                   }},
			                   mRemoteDescription->media(i))) {

				mMidFromSsrc.emplace(ssrc, *found);
				return *found;
			}
		}
	}
	{
		std::lock_guard lock(mLocalDescriptionMutex);
		if (!mLocalDescription)
			return nullopt;
		for (unsigned int i = 0; i < mLocalDescription->mediaCount(); ++i) {
			if (auto found =
			        std::visit(rtc::overloaded{[&](Description::Application *) -> optional<string> {
				                                   return std::nullopt;
			                                   },
			                                   [&](Description::Media *media) -> optional<string> {
				                                   return media->hasSSRC(ssrc)
				                                              ? std::make_optional(media->mid())
				                                              : nullopt;
			                                   }},
			                   mLocalDescription->media(i))) {

				mMidFromSsrc.emplace(ssrc, *found);
				return *found;
			}
		}
	}

	return nullopt;
}

void PeerConnection::forwardBufferedAmount(uint16_t stream, size_t amount) {
	if (auto channel = findDataChannel(stream))
		channel->triggerBufferedAmount(amount);
}

shared_ptr<DataChannel> PeerConnection::emplaceDataChannel(string label, DataChannelInit init) {
	std::unique_lock lock(mDataChannelsMutex); // we are going to emplace
	uint16_t stream;
	if (init.id) {
		stream = *init.id;
		if (stream == 65535)
			throw std::invalid_argument("Invalid DataChannel id");
	} else {
		// RFC 5763: The answerer MUST use either a setup attribute value of setup:active or
		// setup:passive. [...] Thus, setup:active is RECOMMENDED.
		// See https://tools.ietf.org/html/rfc5763#section-5
		// Therefore, we assume passive role if we are the offerer.
		auto iceTransport = getIceTransport();
		auto role = iceTransport ? iceTransport->role() : Description::Role::Passive;

		// RFC 8832: The peer that initiates opening a data channel selects a stream identifier for
		// which the corresponding incoming and outgoing streams are unused.  If the side is acting
		// as the DTLS client, it MUST choose an even stream identifier; if the side is acting as
		// the DTLS server, it MUST choose an odd one.
		// See https://tools.ietf.org/html/rfc8832#section-6
		stream = (role == Description::Role::Active) ? 0 : 1;
		while (mDataChannels.find(stream) != mDataChannels.end()) {
			if (stream >= 65535 - 2)
				throw std::runtime_error("Too many DataChannels");

			stream += 2;
		}
	}
	// If the DataChannel is user-negotiated, do not negociate it here
	auto channel =
	    init.negotiated
	        ? std::make_shared<DataChannel>(weak_from_this(), stream, std::move(label),
	                                        std::move(init.protocol), std::move(init.reliability))
	        : std::make_shared<NegotiatedDataChannel>(weak_from_this(), stream, std::move(label),
	                                                  std::move(init.protocol),
	                                                  std::move(init.reliability));
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

void PeerConnection::shiftDataChannels() {
	auto iceTransport = std::atomic_load(&mIceTransport);
	auto sctpTransport = std::atomic_load(&mSctpTransport);
	if (!sctpTransport && iceTransport && iceTransport->role() == Description::Role::Active) {
		std::unique_lock lock(mDataChannelsMutex); // we are going to swap the container
		decltype(mDataChannels) newDataChannels;
		auto it = mDataChannels.begin();
		while (it != mDataChannels.end()) {
			auto channel = it->second.lock();
			channel->shiftStream();
			newDataChannels.emplace(channel->stream(), channel);
			++it;
		}
		std::swap(mDataChannels, newDataChannels);
	}
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

shared_ptr<Track> PeerConnection::emplaceTrack(Description::Media description) {
	shared_ptr<Track> track;
	if (auto it = mTracks.find(description.mid()); it != mTracks.end())
		if (track = it->second.lock(); track)
			track->setDescription(std::move(description));

	if (!track) {
		track = std::make_shared<Track>(weak_from_this(), std::move(description));
		mTracks.emplace(std::make_pair(track->mid(), track));
		mTrackLines.emplace_back(track);
	}

	return track;
}

void PeerConnection::incomingTrack(Description::Media description) {
	std::unique_lock lock(mTracksMutex); // we are going to emplace
#if !RTC_ENABLE_MEDIA
	if (mTracks.empty()) {
		PLOG_WARNING << "Tracks will be inative (not compiled with media support)";
	}
#endif
	if (mTracks.find(description.mid()) == mTracks.end()) {
		auto track = std::make_shared<Track>(weak_from_this(), std::move(description));
		mTracks.emplace(std::make_pair(track->mid(), track));
		mTrackLines.emplace_back(track);
		triggerTrack(track);
	}
}

void PeerConnection::openTracks() {
#if RTC_ENABLE_MEDIA
	if (auto transport = std::atomic_load(&mDtlsTransport)) {
		auto srtpTransport = std::dynamic_pointer_cast<DtlsSrtpTransport>(transport);
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
		throw std::invalid_argument("Remote description has no valid fingerprint");

	if (description.mediaCount() == 0)
		throw std::invalid_argument("Remote description has no media line");

	int activeMediaCount = 0;
	for (unsigned int i = 0; i < description.mediaCount(); ++i)
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
	const uint16_t localSctpPort = DEFAULT_SCTP_PORT;
	const size_t localMaxMessageSize =
	    config.maxMessageSize.value_or(DEFAULT_LOCAL_MAX_MESSAGE_SIZE);

	// Clean up the application entry the ICE transport might have added already (libnice)
	description.clearMedia();

	if (auto remote = remoteDescription()) {
		// Reciprocate remote description
		for (unsigned int i = 0; i < remote->mediaCount(); ++i)
			std::visit( // reciprocate each media
			    rtc::overloaded{
			        [&](Description::Application *remoteApp) {
				        std::shared_lock lock(mDataChannelsMutex);
				        if (!mDataChannels.empty()) {
					        // Prefer local description
					        Description::Application app(remoteApp->mid());
					        app.setSctpPort(localSctpPort);
					        app.setMaxMessageSize(localMaxMessageSize);

					        PLOG_DEBUG << "Adding application to local description, mid=\""
					                   << app.mid() << "\"";

					        description.addMedia(std::move(app));
					        return;
				        }

				        auto reciprocated = remoteApp->reciprocate();
				        reciprocated.hintSctpPort(localSctpPort);
				        reciprocated.setMaxMessageSize(localMaxMessageSize);

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
				        lock.unlock(); // we are going to call incomingTrack()

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
				unsigned int m = 0;
				while (description.hasMid(std::to_string(m)))
					++m;
				Description::Application app(std::to_string(m));
				app.setSctpPort(localSctpPort);
				app.setMaxMessageSize(localMaxMessageSize);

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

		mLocalDescription.emplace(description);
		mLocalDescription->addCandidates(std::move(existingCandidates));
	}

	PLOG_VERBOSE << "Issuing local description: " << description;
	mProcessor->enqueue(localDescriptionCallback.wrap(), std::move(description));

	// Reciprocated tracks might need to be open
	if (auto dtlsTransport = std::atomic_load(&mDtlsTransport);
	    dtlsTransport && dtlsTransport->state() == Transport::State::Connected)
		mProcessor->enqueue(&PeerConnection::openTracks, this);
}

void PeerConnection::processLocalCandidate(Candidate candidate) {
	std::lock_guard lock(mLocalDescriptionMutex);
	if (!mLocalDescription)
		throw std::logic_error("Got a local candidate without local description");

	candidate.resolve(Candidate::ResolveMode::Simple);
	mLocalDescription->addCandidate(candidate);

	PLOG_VERBOSE << "Issuing local candidate: " << candidate;
	mProcessor->enqueue(localCandidateCallback.wrap(), std::move(candidate));
}

void PeerConnection::processRemoteDescription(Description description) {
	{
		// Set as remote description
		std::lock_guard lock(mRemoteDescriptionMutex);

		std::vector<Candidate> existingCandidates;
		if (mRemoteDescription)
			existingCandidates = mRemoteDescription->extractCandidates();

		mRemoteDescription.emplace(description);
		mRemoteDescription->addCandidates(std::move(existingCandidates));
	}

	auto iceTransport = initIceTransport();
	iceTransport->setRemoteDescription(std::move(description));

	// Since we assumed passive role during DataChannel creation, we might need to shift the stream
	// numbers from odd to even.
	shiftDataChannels();

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
	{
		// Set as remote candidate
		std::lock_guard lock(mRemoteDescriptionMutex);
		if (!mRemoteDescription)
			throw std::logic_error("Got a remote candidate without remote description");

		if (!iceTransport)
			throw std::logic_error("Got a remote candidate without ICE transport");

		candidate.hintMid(mRemoteDescription->bundleMid());

		if (mRemoteDescription->hasCandidate(candidate))
			return; // already in description, ignore

		candidate.resolve(Candidate::ResolveMode::Simple);
		mRemoteDescription->addCandidate(candidate);
	}

	if (candidate.isResolved()) {
		iceTransport->addRemoteCandidate(std::move(candidate));
	} else {
		// We might need a lookup, do it asynchronously
		// We don't use the thread pool because we have no control on the timeout
		if ((iceTransport = std::atomic_load(&mIceTransport))) {
			weak_ptr<IceTransport> weakIceTransport{iceTransport};
			std::thread t([weakIceTransport, candidate = std::move(candidate)]() mutable {
				if (candidate.resolve(Candidate::ResolveMode::Lookup))
					if (auto iceTransport = weakIceTransport.lock())
						iceTransport->addRemoteCandidate(std::move(candidate));
			});
			t.detach();
		}
	}
}

string PeerConnection::localBundleMid() const {
	std::lock_guard lock(mLocalDescriptionMutex);
	return mLocalDescription ? mLocalDescription->bundleMid() : "0";
}

void PeerConnection::triggerDataChannel(weak_ptr<DataChannel> weakDataChannel) {
	auto dataChannel = weakDataChannel.lock();
	if (dataChannel) {
		dataChannel->resetOpenCallback(); // might be set internally
		mPendingDataChannels.push(std::move(dataChannel));
	}
	triggerPendingDataChannels();
}

void PeerConnection::triggerTrack(weak_ptr<Track> weakTrack) {
	auto track = weakTrack.lock();
	if (track) {
		track->resetOpenCallback(); // might be set internally
		mPendingTracks.push(std::move(track));
	}
	triggerPendingTracks();
}

void PeerConnection::triggerPendingDataChannels() {
	while (dataChannelCallback) {
		auto next = mPendingDataChannels.tryPop();
		if (!next)
			break;

		auto impl = std::move(*next);
		dataChannelCallback(std::make_shared<rtc::DataChannel>(impl));
		impl->triggerOpen();
	}
}

void PeerConnection::triggerPendingTracks() {
	while (trackCallback) {
		auto next = mPendingTracks.tryPop();
		if (!next)
			break;

		auto impl = std::move(*next);
		trackCallback(std::make_shared<rtc::Track>(impl));
		impl->triggerOpen();
	}
}

void PeerConnection::flushPendingDataChannels() {
	mProcessor->enqueue(std::bind(&PeerConnection::triggerPendingDataChannels, this));
}

void PeerConnection::flushPendingTracks() {
	mProcessor->enqueue(std::bind(&PeerConnection::triggerPendingTracks, this));
}

bool PeerConnection::changeState(State newState) {
	State current;
	do {
		current = state.load();
		if (current == State::Closed)
			return false;
		if (current == newState)
			return false;

	} while (!state.compare_exchange_weak(current, newState));

	std::ostringstream s;
	s << newState;
	PLOG_INFO << "Changed state to " << s.str();

	if (newState == State::Closed)
		// This is the last state change, so we may steal the callback
		mProcessor->enqueue([cb = std::move(stateChangeCallback)]() { cb(State::Closed); });
	else
		mProcessor->enqueue(stateChangeCallback.wrap(), newState);

	return true;
}

bool PeerConnection::changeGatheringState(GatheringState newState) {
	if (gatheringState.exchange(newState) == newState)
		return false;

	std::ostringstream s;
	s << newState;
	PLOG_INFO << "Changed gathering state to " << s.str();
	mProcessor->enqueue(gatheringStateChangeCallback.wrap(), newState);
	return true;
}

bool PeerConnection::changeSignalingState(SignalingState newState) {
	if (signalingState.exchange(newState) == newState)
		return false;

	std::ostringstream s;
	s << state;
	PLOG_INFO << "Changed signaling state to " << s.str();
	mProcessor->enqueue(signalingStateChangeCallback.wrap(), newState);
	return true;
}

void PeerConnection::resetCallbacks() {
	// Unregister all callbacks
	dataChannelCallback = nullptr;
	localDescriptionCallback = nullptr;
	localCandidateCallback = nullptr;
	stateChangeCallback = nullptr;
	gatheringStateChangeCallback = nullptr;
}

} // namespace rtc::impl
