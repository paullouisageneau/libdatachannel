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
#include "certificate.hpp"
#include "datachannel.hpp"
#include "description.hpp"
#include "iceconfiguration.hpp"
#include "include.hpp"
#include "message.hpp"
#include "reliability.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

namespace rtc {

class IceTransport;
class DtlsTransport;
class SctpTransport;

class PeerConnection {
public:
	PeerConnection(const IceConfiguration &config);
	~PeerConnection();

	const IceConfiguration *config() const;
	const Certificate *certificate() const;

	std::optional<Description> localDescription() const;
	std::optional<Description> remoteDescription() const;

	void setRemoteDescription(Description description);
	void setRemoteCandidate(Candidate candidate);

	std::shared_ptr<DataChannel> createDataChannel(const string &label, const string &protocol = "",
	                                               const Reliability &reliability = {});

	void onDataChannel(std::function<void(std::shared_ptr<DataChannel> dataChannel)> callback);
	void onLocalDescription(std::function<void(const Description &description)> callback);
	void onLocalCandidate(std::function<void(const std::optional<Candidate> &candidate)> callback);

private:
	void initIceTransport(Description::Role role);
	void initDtlsTransport();
	void initSctpTransport();

	bool checkFingerprint(const std::string &fingerprint) const;
	void forwardMessage(message_ptr message);
	void openDataChannels(void);

	void processLocalDescription(Description description);
	void processLocalCandidate(std::optional<Candidate> candidate);
	void triggerDataChannel(std::shared_ptr<DataChannel> dataChannel);

	const IceConfiguration mConfig;
	const Certificate mCertificate;

	std::optional<Description> mLocalDescription;
	std::optional<Description> mRemoteDescription;

	std::shared_ptr<IceTransport> mIceTransport;
	std::shared_ptr<DtlsTransport> mDtlsTransport;
	std::shared_ptr<SctpTransport> mSctpTransport;

	std::unordered_map<unsigned int, std::shared_ptr<DataChannel>> mDataChannels;

	std::function<void(std::shared_ptr<DataChannel> dataChannel)> mDataChannelCallback;
	std::function<void(const Description &description)> mLocalDescriptionCallback;
	std::function<void(const std::optional<Candidate> &candidate)> mLocalCandidateCallback;
};

} // namespace rtc

#endif
