/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// SFrame receiver: answers the sender's offer, decrypts its audio and video, and sends its own
// media back under its own keys. Pair with the sender in this directory.

#include "sframecommon.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

int main() {
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::Configuration config;
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	pc->onLocalDescription(printLocalDescription);
	pc->onLocalCandidate(printLocalCandidate);
	pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[State: " << state << "]" << std::endl;
	});

	const std::string cname = "sframe-receiver";
	const rtc::SSRC videoSsrc = 52;
	const rtc::SSRC audioSsrc = 54;

	// Decrypt-only by construction: an SFrameReceiveKey can only come from here, so this end's own
	// sending keys cannot be handed back by mistake. One provider per m-line, because the two
	// m-lines use different cipher suites and a provider fixes its suite at construction.
	//
	// Both start empty -- the peer's keys arrive through command 3 -- so until they do, the peer's
	// frames are refused rather than guessed at.
	//
	// KID lifecycle is entirely the application's: the library derives keys, encrypts, and looks
	// them up by generation, but it never invents, reuses or retires a KID on its own. Across a key
	// roll, addKey() the new generation and removeKey() the old one only once its frames have
	// drained. Nothing checks that a (key, counter) pair is never repeated either -- that
	// obligation sits with whatever issues the keys.
	auto videoReceiveProvider = std::make_shared<rtc::SFrameReceiveKeyProvider>(
	    kVideoCipherSuite, kRatchetStepBits, /*perSsrcDerivation=*/true);
	auto audioReceiveProvider = std::make_shared<rtc::SFrameReceiveKeyProvider>(
	    kAudioCipherSuite, kRatchetStepBits, /*perSsrcDerivation=*/true);

	// A provider holds any number of generations at once, so a multi-party call can register every
	// participant's key up front and one provider serves every track. Nothing is dropped on its
	// own, so call removeKey() when a participant leaves. The library cannot decide that: behind an
	// SSRC-rewriting SFU a change of generation means the speaker changed rather than that anyone
	// rekeyed, and the SSRC is the only stream identity it has.

	// This end sends too, under keys of its own that the peer never chooses. RFC 9605 Section 4.4.1
	// again: no base key is shared between the two directions, or between the two suites.
	auto videoSendKey = generateSendKey();
	auto audioSendKey = generateSendKey();
	printSendKey(Kind::Video, videoSendKey);
	printSendKey(Kind::Audio, audioSendKey);

	auto videoSendProvider = std::make_shared<rtc::SFrameSendKeyProvider>(
	    kVideoCipherSuite, kRatchetStepBits, kRatchetPeriod, /*perSsrcDerivation=*/true,
	    rtc::SFrameSendKey{videoSendKey.baseKey, videoSendKey.kid});
	auto audioSendProvider = std::make_shared<rtc::SFrameSendKeyProvider>(
	    kAudioCipherSuite, kRatchetStepBits, kRatchetPeriod, /*perSsrcDerivation=*/true,
	    rtc::SFrameSendKey{audioSendKey.baseKey, audioSendKey.kid});

	std::atomic<int> videoReceived{0};
	std::atomic<int> audioReceived{0};

	// Codecs this end can handle, most preferred first. The answerer is the one that picks --
	// nothing in the library narrows a codec list. Video prefers VP8 over H.264 deliberately, the
	// reverse of the order the sender offers them in, so that running the pair demonstrates the
	// offerer really reading the choice out of the answer rather than assuming its own first
	// preference.
	const std::vector<std::string> videoPreference = {"VP8", "H264"};
	const std::vector<std::string> audioPreference = {"opus"};

	// Published from the onTrack callback thread and read by the send loop, so the handoff is
	// atomic. The payload types stay zero until a chain has been built, which is what tells the
	// send loop that the codec is settled.
	std::shared_ptr<rtc::Track> videoTrack, audioTrack;
	std::atomic<uint8_t> videoSendPayloadType{0};
	std::atomic<uint8_t> audioSendPayloadType{0};

	// Must be registered before any offer is applied: the answer is produced inside
	// setRemoteDescription(), so a later callback runs too late and the m-line is already stopped.
	pc->onTrack([&](std::shared_ptr<rtc::Track> track) {
		const bool isVideo = track->description().type() == "video";
		std::cout << "[Track: mid=" << track->mid() << " (" << track->description().type() << ")]"
		          << std::endl;

		// Narrowing the answer to one codec, which is the answerer's job and nobody else's:
		// reciprocate() copies every a=rtpmap the offer carried into this description, and the
		// answer is then built from it verbatim, so an answerer that leaves the list alone accepts
		// every codec offered and gives the offerer no way to tell which one to send.
		uint8_t chosenPayloadType = 0;
		std::string chosenFormat;
		{
			auto desc = track->description();
			for (const auto &codec : isVideo ? videoPreference : audioPreference) {
				for (int payloadType : desc.payloadTypes()) {
					// A static payload type carries no a=rtpmap (RFC 4566) and rtpMap() throws for
					// it.
					if (!desc.hasPayloadType(payloadType))
						continue;
					if (desc.rtpMap(payloadType)->format == codec) {
						chosenPayloadType = uint8_t(payloadType);
						chosenFormat = codec;
						break;
					}
				}
				if (chosenPayloadType)
					break;
			}

			if (chosenPayloadType == 0) {
				// Nothing in common. Marking the m-line removed rejects it in the answer, and the
				// library closes the track once this callback returns.
				std::cout << "[No codec in common on mid=" << track->mid() << ", rejecting it]"
				          << std::endl;
				desc.markRemoved();
				track->setDescription(std::move(desc));
				return;
			}

			// Everything else goes -- except an RTX mapping for the codec being kept, which is not
			// a competing codec but a retransmission channel for this one. Removing the other
			// codecs takes their RTX mappings with them, since removeRtpMap() follows apt.
			const std::string keepApt = "apt=" + std::to_string(chosenPayloadType);
			for (int payloadType : desc.payloadTypes()) {
				if (payloadType == int(chosenPayloadType))
					continue;
				const auto *map =
				    desc.hasPayloadType(payloadType) ? desc.rtpMap(payloadType) : nullptr;
				if (map && (map->format == "rtx" || map->format == "RTX") &&
				    std::find(map->fmtps.begin(), map->fmtps.end(), keepApt) != map->fmtps.end())
					continue;
				desc.removeRtpMap(payloadType);
			}

			// This m-line is sendrecv, so this end sends on it too, and the answer has to advertise
			// the SSRC it will send on. reciprocate() clears the offer's SSRCs -- they described
			// the offerer's streams, not ours -- so without this the answer names no stream of our
			// own. The peer then has nothing to map our RTP onto: it arrives, matches no track, and
			// is dropped, so our media silently never reaches the application at the far end.
			desc.addSSRC(isVideo ? videoSsrc : audioSsrc, cname);

			track->setDescription(std::move(desc));
		}

		std::cout << "[Chose " << chosenFormat << ", payload type " << unsigned(chosenPayloadType)
		          << " on mid=" << track->mid() << "]" << std::endl;

		// The packetizer is the application's to build -- it needs an SSRC and a payload type only
		// the application knows -- and now that the codec is settled, the payload type is the one
		// this answer commits to.
		auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		    isVideo ? videoSsrc : audioSsrc, cname, chosenPayloadType,
		    isVideo ? rtc::RtpPacketizer::VideoClockRate
		            : rtc::OpusRtpPacketizer::DefaultClockRate);

		if (!track->description().hasSFrame()) {
			// No a=sframe on this m-line, so the peer never asked for protection and this is an
			// ordinary track -- not a declined one. It needs the negotiated codec's own handlers,
			// the stage SFrame stands in for when it is negotiated. Note there is no halfway
			// state: an SFrame handler never passes plaintext through, so a track either has the
			// codec's handlers as here, or SFrame's, and never a mix.
			std::cout << "[Peer did not offer SFrame on mid=" << track->mid()
			          << "; media on this track will NOT be protected]" << std::endl;

			if (!isVideo)
				track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
			else if (chosenFormat == "VP8")
				track->setMediaHandler(std::make_shared<rtc::VP8RtpDepacketizer>());
			else
				track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>());

			track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

			// The m-line is sendrecv, so this end sends too, and unprotected media goes out through
			// the codec's own packetizer.
			if (!isVideo)
				track->chainMediaHandler(std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig));
			else if (chosenFormat == "VP8")
				track->chainMediaHandler(std::make_shared<rtc::VP8RtpPacketizer>(rtpConfig));
			else
				track->chainMediaHandler(std::make_shared<rtc::H264RtpPacketizer>(
				    rtc::NalUnit::Separator::Length, rtpConfig));

			std::atomic_store(isVideo ? &videoTrack : &audioTrack, track);
			(isVideo ? videoSendPayloadType : audioSendPayloadType) = chosenPayloadType;
			return;
		}

		// Per track rather than PeerConnection::useSFrame(), because that registers one provider
		// for the whole session and the two m-lines use different cipher suites. Called here, in
		// the track callback, so the answer keeps a=sframe: the attribute survives only while a
		// handler in the chain applies SFrame.
		track->useSFrame(isVideo ? videoReceiveProvider : audioReceiveProvider);

		// Chained after SFrame, so on the way in it runs first -- incoming handlers run from the
		// tail of the chain toward the head. That unwraps an RTX retransmission, restoring the
		// original SSRC and sequence number, before SFrame reads the descriptor byte. It also
		// carries NACK, PLI and RTCP report handling.
		track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

		// The m-line is sendrecv, so this end sends too. Chaining the packetizer leaves the
		// depacketizer in place: one handles outgoing, the other incoming. SFrame's packetizer is
		// codec-agnostic, so the codec chosen above changes only the payload type it stamps.
		track->chainMediaHandler(std::make_shared<rtc::SFramePerFrameRtpPacketizer>(
		    rtpConfig, isVideo ? videoSendProvider : audioSendProvider));

		auto *counter = isVideo ? &videoReceived : &audioReceived;
		const std::string label = isVideo ? "video" : "audio";
		track->onFrame([counter, label](rtc::binary frame, rtc::FrameInfo info) {
			std::cout << "[Decrypted " << label << " frame " << (*counter)++ << ": " << frame.size()
			          << " bytes, timestamp " << info.timestamp << "]" << std::endl;
		});

		std::atomic_store(isVideo ? &videoTrack : &audioTrack, track);
		(isVideo ? videoSendPayloadType : audioSendPayloadType) = chosenPayloadType;
	});

	std::atomic<bool> stop{false};
	std::thread sender([&]() {
		uint32_t videoTimestamp = 0, audioTimestamp = 0;
		int counter = 0;
		while (!stop) {
			const uint8_t videoPt = videoSendPayloadType.load();
			if (auto track = std::atomic_load(&videoTrack); videoPt && track && track->isOpen()) {
				// Stand-in for encoder output: a counter the far end can check.
				rtc::binary frame(1200);
				for (size_t i = 0; i < frame.size(); ++i)
					frame[i] = std::byte(uint8_t(counter + i));

				rtc::FrameInfo info(videoTimestamp);
				info.payloadType = videoPt;
				try {
					track->sendFrame(std::move(frame), info);
					std::cout << "[Sent video frame " << counter << "]" << std::endl;
				} catch (const std::exception &e) {
					std::cout << "[Video send failed: " << e.what() << "]" << std::endl;
				}
				videoTimestamp += 3000; // 90 kHz clock, ~30 fps
			}

			const uint8_t audioPt = audioSendPayloadType.load();
			if (auto track = std::atomic_load(&audioTrack); audioPt && track && track->isOpen()) {
				// One Opus-sized frame, small enough to need no fragmenting.
				rtc::binary frame(160);
				for (size_t i = 0; i < frame.size(); ++i)
					frame[i] = std::byte(uint8_t(counter * 2 + i));

				rtc::FrameInfo info(audioTimestamp);
				info.payloadType = audioPt;
				try {
					track->sendFrame(std::move(frame), info);
				} catch (const std::exception &e) {
					std::cout << "[Audio send failed: " << e.what() << "]" << std::endl;
				}
				audioTimestamp += 960; // 48 kHz clock, 20 ms
			}

			counter++;
			std::this_thread::sleep_for(33ms);
		}
	});

	runSignalling(pc, nullptr, [&](Kind kind, const KeyMaterial &peer) {
		// Decryption only, and refused outright if it collides with the key we send under.
		if (kind == Kind::Video)
			installPeerKey(videoReceiveProvider, videoSendKey, peer);
		else
			installPeerKey(audioReceiveProvider, audioSendKey, peer);
	});

	stop = true;
	sender.join();
	std::cout << "Decrypted " << videoReceived << " video and " << audioReceived << " audio frames"
	          << std::endl;
	pc->close();
	return 0;
}
