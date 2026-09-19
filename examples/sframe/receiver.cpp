/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// SFrame receiver: answers the sender's offer and decrypts the incoming video track.
// Pair with the sender in this directory.

#include "common.hpp"

int main() {
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::Configuration config;
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	pc->onLocalDescription(printLocalDescription);
	pc->onLocalCandidate(printLocalCandidate);
	pc->onStateChange(
	    [](rtc::PeerConnection::State state) { std::cout << "[State: " << state << "]" << std::endl; });

	// Decrypt-only: this holds the sender's key generation and key, and is never used to
	// encrypt. If this peer also sent media it would need its own separate base key and key
	// generation (kReceiverKeyGeneration / receiverBaseKey), not these.
	auto keyProvider =
	    std::make_shared<PeerKeyProvider>(kSenderKeyGeneration, senderBaseKey());
	std::atomic<int> received{0};

	pc->onTrack([&](std::shared_ptr<rtc::Track> track) {
		std::cout << "[Track: mid=" << track->mid() << "]" << std::endl;

		// The offer's a=sframe is carried through to here, so this is where to decide. If the
		// peer did not ask for SFrame, install nothing: the attribute is dropped from the
		// answer automatically, because no handler in the chain applies SFrame.
		if (!track->description().hasSFrame()) {
			std::cout << "[Peer did not offer SFrame on mid=" << track->mid()
			          << "; media on this track will NOT be protected]" << std::endl;
			return;
		}

		// Decrypts and reassembles. The key provider is mandatory: without it the handler
		// could only pass ciphertext up as if it were media. Installing it is also what keeps
		// a=sframe in the answer.
		track->setMediaHandler(std::make_shared<rtc::SFrameVideoRtpDepacketizer>(keyProvider));

		// A depacketizer stamps FrameInfo on what it produces, so decrypted frames arrive
		// through onFrame. onMessage would only see traffic without FrameInfo, such as RTCP.
		track->onFrame([&received](rtc::binary frame, rtc::FrameInfo info) {
			std::cout << "[Decrypted frame " << received++ << ": " << frame.size()
			          << " bytes, timestamp " << info.timestamp << "]" << std::endl;
		});
	});

	runSignalling(pc);

	std::cout << "Decrypted " << received << " frames" << std::endl;
	pc->close();
	return 0;
}
