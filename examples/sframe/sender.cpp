/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// SFrame sender: offers a video track with a=sframe and encrypts every frame end to end.
// Pair with the receiver in this directory.

#include "common.hpp"

int main() {
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::Configuration config;
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	pc->onLocalDescription(printLocalDescription);
	pc->onLocalCandidate(printLocalCandidate);
	pc->onStateChange(
	    [](rtc::PeerConnection::State state) { std::cout << "[State: " << state << "]" << std::endl; });

	const rtc::SSRC ssrc = 42;
	const uint8_t payloadType = 96;
	const std::string cname = "sframe-sender";

	rtc::Description::Video media(cname, rtc::Description::Direction::SendOnly);
	media.addH264Codec(payloadType);
	media.addSSRC(ssrc, cname);

	// Advertise SFrame on this m-line. If the answer comes back without it the packetizer
	// stops encrypting, so check hasSFrame() before assuming the media is protected.
	media.addSFrame();

	auto track = pc->addTrack(media);

	rtc::SFrameConfig sframeConfig;
	sframeConfig.keyDetails.cipherSuiteId = kCipherSuite;
	// The sender's own key, never the one it decrypts with: RFC 9605 Section 4.4.1 requires
	// a base key be marked for encryption or decryption, never both.
	sframeConfig.keyDetails.baseKey = senderBaseKey();
	sframeConfig.keyDetails.use = rtc::SFrameKeyUse::Encrypt;
	sframeConfig.keyGeneration = kSenderKeyGeneration;
	sframeConfig.ratchetStepBits = kRatchetStepBits;
	sframeConfig.ratchetPeriod = kRatchetPeriod;
	// Must match PeerKeyProvider::usePerSSRCDerivation() on the receiver. Not negotiated in
	// SDP, so it is a deployment setting like the cipher suite.
	sframeConfig.perSsrcDerivation = true;

	auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
	    ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);

	// Replaces the codec packetizer: the frame is encrypted whole, then split into MTU-sized
	// chunks each carrying the 1-byte SFrame descriptor, so no codec-specific fragmentation
	// happens and the same packetizer serves audio and video.
	auto packetizer = std::make_shared<rtc::SFrameRtpPacketizer>(rtpConfig, sframeConfig,
	                                                             rtc::SFrameMode::PerFrame);
	track->setMediaHandler(packetizer);

	std::atomic<bool> stop{false};
	std::thread sender([&]() {
		uint32_t timestamp = 0;
		int counter = 0;
		while (!stop) {
			if (track->isOpen()) {
				// Stand-in for encoder output: a counter the receiver can check.
				rtc::binary frame(1200);
				for (size_t i = 0; i < frame.size(); ++i)
					frame[i] = std::byte(uint8_t(counter + i));

				rtc::FrameInfo info(timestamp);
				info.payloadType = payloadType;
				try {
					track->sendFrame(std::move(frame), info);
					std::cout << "[Sent frame " << counter << "]" << std::endl;
				} catch (const std::exception &e) {
					std::cout << "[Send failed: " << e.what() << "]" << std::endl;
				}
				++counter;
				timestamp += 3000; // 30 fps at a 90 kHz clock
			}
			std::this_thread::sleep_for(33ms);
		}
	});

	pc->setLocalDescription();
	runSignalling(pc);

	stop = true;
	sender.join();
	pc->close();
	return 0;
}
