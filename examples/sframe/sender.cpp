/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// SFrame sender: offers an audio and a video track, both SFrame protected, and decrypts what the
// receiver sends back. Pair with the receiver in this directory.

#include "sframecommon.hpp"

#include <atomic>
#include <thread>
#include <variant>

int main() {
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::Configuration config;
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	pc->onLocalDescription(printLocalDescription);
	pc->onLocalCandidate(printLocalCandidate);
	pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[State: " << state << "]" << std::endl;
	});

	const std::string cname = "sframe-sender";
	const uint8_t videoPayloadType = 96; // H.264
	const uint8_t rtxPayloadType = 97;
	const uint8_t vp8PayloadType = 98; // VP8, offered as an alternative to H.264
	const uint8_t audioPayloadType = 111;
	const rtc::SSRC videoSsrc = 42;
	const rtc::SSRC rtxSsrc = 43;
	const rtc::SSRC audioSsrc = 44;

	// Both m-lines are sendrecv, so media flows in both directions and each direction carries its
	// own key. Each m-line also gets its own cipher suite -- see the cipher suite constants for why
	// the tag lengths differ between audio and video.
	rtc::Description::Video videoMedia("video", rtc::Description::Direction::SendRecv);
	videoMedia.addH264Codec(videoPayloadType);

	// A second video codec, so the answer has a real choice to make. Narrowing a codec list is the
	// answerer's job and nobody else's: libdatachannel answers with every codec it was offered
	// unless the application removes the ones it will not use. So this end cannot know which codec
	// it is sending until it has read the answer, which is one of the two reasons the media chain
	// below is built late.
	videoMedia.addVP8Codec(vp8PayloadType);

	videoMedia.addSSRC(videoSsrc, cname);

	// RTX on the video m-line, so a lost frame can be retransmitted per RFC 4588. It works with
	// SFrame because the receiver unwraps the retransmission back to the original SSRC and sequence
	// number before SFrame sees it, so a recovered frame decrypts like any other.
	//
	// This mapping names H.264 through its apt parameter, and it is bound to that codec alone: an
	// answerer that narrows to VP8 drops this mapping along with H.264 -- removeRtpMap() also
	// removes any RTX mapping whose apt names the payload type going away -- and the library then
	// disables RTX on this track. An application wanting RTX whichever codec is chosen adds one
	// mapping per codec.
	videoMedia.addRtxCodec(rtxPayloadType, videoPayloadType, rtc::H264RtpPacketizer::ClockRate);
	videoMedia.addRtxSSRC(videoSsrc, rtxSsrc, cname);

	// Advertise SFrame on each m-line. If the answer comes back without it the library stops that
	// m-line rather than sending in the clear, so the track closes and onClosed() fires for it.
	videoMedia.addSFrame();

	rtc::Description::Audio audioMedia("audio", rtc::Description::Direction::SendRecv);
	audioMedia.addOpusCodec(audioPayloadType);
	audioMedia.addSSRC(audioSsrc, cname);
	audioMedia.addSFrame();

	auto videoTrack = pc->addTrack(videoMedia);
	auto audioTrack = pc->addTrack(audioMedia);

	// A key per m-line per direction. Nothing is shared: not between the two suites, and not
	// between the two directions (RFC 9605 Section 4.4.1).
	auto videoSendKey = generateSendKey();
	auto audioSendKey = generateSendKey();
	printSendKey(Kind::Video, videoSendKey);
	printSendKey(Kind::Audio, audioSendKey);

	// The sending key, never one this end decrypts with: an SFrameSendKey can only come from here,
	// so the two cannot be crossed. perSsrcDerivation must match the far end -- it is not
	// negotiated in SDP, so it is a deployment setting like the cipher suite.
	//
	// Call rollKey() to move to new key material later. Which KIDs to use, and when to stop using
	// one, are the application's: rollKey() accepts a KID it has sent under before so long as the
	// material is new, which is what the RFC 9605 Section 5.2 MLS layout needs -- that KID carries
	// only the low bits of the MLS epoch, so its values necessarily recur. Handing it back the key
	// already in force is simply ignored, so a stray re-supply cannot restart the counter and
	// replay nonces. What it cannot check is a key re-supplied with a counter set too low.
	//
	// Resuming is per mode, and this example runs with per-SSRC derivation on, where currentKey()
	// reports nullopt: the counter belongs to each track's derived key rather than to the session,
	// so there is no single point to resume from. Roll to a key generation never used before
	// instead -- the KID is an HKDF input to both the key and the salt, so a new generation is a
	// fresh nonce space and nothing can repeat. With derivation off there is one counter for the
	// session, and then currentKey() is what to persist and hand to the constructor on resume,
	// rather than rolling to it.
	auto videoSendProvider = std::make_shared<rtc::SFrameSendKeyProvider>(
	    kVideoCipherSuite, kRatchetStepBits, kRatchetPeriod, /*perSsrcDerivation=*/true,
	    rtc::SFrameSendKey{videoSendKey.baseKey, videoSendKey.kid});
	auto audioSendProvider = std::make_shared<rtc::SFrameSendKeyProvider>(
	    kAudioCipherSuite, kRatchetStepBits, kRatchetPeriod, /*perSsrcDerivation=*/true,
	    rtc::SFrameSendKey{audioSendKey.baseKey, audioSendKey.kid});

	// Separate providers for the other direction, holding only what the peer sends. They start
	// empty: the peer's keys arrive through command 3, and until they do the peer's frames are
	// refused rather than guessed at.
	//
	// Installed per track rather than through PeerConnection::useSFrame(), because that registers
	// one provider for the whole session and the two m-lines use different cipher suites.
	auto videoReceiveProvider = std::make_shared<rtc::SFrameReceiveKeyProvider>(
	    kVideoCipherSuite, kRatchetStepBits, /*perSsrcDerivation=*/true);
	auto audioReceiveProvider = std::make_shared<rtc::SFrameReceiveKeyProvider>(
	    kAudioCipherSuite, kRatchetStepBits, /*perSsrcDerivation=*/true);

	// Decrypted frames from the peer arrive through onFrame: a depacketizer stamps FrameInfo on
	// what it produces, and only traffic without it, such as RTCP, reaches onMessage.
	std::atomic<int> videoReceived{0};
	std::atomic<int> audioReceived{0};
	videoTrack->onFrame([&videoReceived](rtc::binary frame, rtc::FrameInfo info) {
		std::cout << "[Decrypted video frame " << videoReceived++ << ": " << frame.size()
		          << " bytes, timestamp " << info.timestamp << "]" << std::endl;
	});
	audioTrack->onFrame([&audioReceived](rtc::binary frame, rtc::FrameInfo info) {
		std::cout << "[Decrypted audio frame " << audioReceived++ << ": " << frame.size()
		          << " bytes, timestamp " << info.timestamp << "]" << std::endl;
	});

	// The chains are built from the answer because the answer settles the codec, and the packetizer
	// needs its payload type. That is the only reason left: SFrame itself cannot be declined part
	// way through, since an answer without a=sframe stops the m-line rather than downgrading it.
	// With one codec per m-line there is nothing left to wait for and the chain can be built before
	// the offer -- see "When to build the media chain" in README.md.

	// Set once the answer has been read, and non-zero only from then on. The send loop waits for
	// them, so no frame is ever packetized for a codec the answer did not choose.
	std::atomic<uint8_t> videoSendPayloadType{0};
	std::atomic<uint8_t> audioSendPayloadType{0};

	auto buildVideoChain = [&](uint8_t payloadType) {
		auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		    videoSsrc, cname, payloadType, rtc::RtpPacketizer::VideoClockRate);

		// Receive side first: useSFrame() puts the SFrame depacketizer at the head of the chain
		// and keeps whatever is already there behind it.
		videoTrack->useSFrame(videoReceiveProvider);

		// Stands in for the codec packetizer, whichever codec was chosen: the frame is encrypted
		// whole, then split into MTU-sized chunks each carrying the 1-byte SFrame descriptor.
		videoTrack->chainMediaHandler(
		    std::make_shared<rtc::SFramePerFrameRtpPacketizer>(rtpConfig, videoSendProvider));

		// Answers a NACK with a retransmission on the RTX SSRC. Chained last, so outgoing frames
		// are encrypted and framed before it stores what actually went on the wire.
		videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpNackResponder>());
		videoSendPayloadType = payloadType;
	};

	auto buildAudioChain = [&](uint8_t payloadType) {
		auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		    audioSsrc, cname, payloadType, rtc::OpusRtpPacketizer::DefaultClockRate);

		audioTrack->useSFrame(audioReceiveProvider);
		audioTrack->chainMediaHandler(
		    std::make_shared<rtc::SFramePerFrameRtpPacketizer>(rtpConfig, audioSendProvider));
		audioSendPayloadType = payloadType;
	};

	std::atomic<bool> stop{false};
	std::thread sender([&]() {
		uint32_t videoTimestamp = 0, audioTimestamp = 0;
		int counter = 0;
		while (!stop) {
			// Zero until the answer has been read and the chain built, so nothing is sent for a
			// codec the peer did not accept.
			const uint8_t videoPt = videoSendPayloadType.load();
			if (videoPt && videoTrack->isOpen()) {
				// Stand-in for encoder output: a counter the receiver can check.
				rtc::binary frame(1200);
				for (size_t i = 0; i < frame.size(); ++i)
					frame[i] = std::byte(uint8_t(counter + i));

				rtc::FrameInfo info(videoTimestamp);
				info.payloadType = videoPt;
				try {
					videoTrack->sendFrame(std::move(frame), info);
					std::cout << "[Sent video frame " << counter << "]" << std::endl;
				} catch (const std::exception &e) {
					std::cout << "[Video send failed: " << e.what() << "]" << std::endl;
				}
				videoTimestamp += 3000; // 90 kHz clock, ~30 fps
			}

			const uint8_t audioPt = audioSendPayloadType.load();
			if (audioPt && audioTrack->isOpen()) {
				// One Opus-sized frame, small enough to need no fragmenting.
				rtc::binary frame(160);
				for (size_t i = 0; i < frame.size(); ++i)
					frame[i] = std::byte(uint8_t(counter * 2 + i));

				rtc::FrameInfo info(audioTimestamp);
				info.payloadType = audioPt;
				try {
					audioTrack->sendFrame(std::move(frame), info);
				} catch (const std::exception &e) {
					std::cout << "[Audio send failed: " << e.what() << "]" << std::endl;
				}
				audioTimestamp += 960; // 48 kHz clock, 20 ms
			}

			counter++;
			std::this_thread::sleep_for(33ms);
		}
	});

	// Generates the offer, which onLocalDescription prints for the other peer. addTrack() only
	// registers the m-line; nothing is negotiated until this is called. The answerer needs no
	// equivalent -- applying an offer produces its answer.
	pc->setLocalDescription();

	// There is no decision to make about a peer that answers without a=sframe: the library stops
	// that m-line rather than downgrading it, so the track closes and onClosed fires for it. Media
	// offered as end-to-end encrypted is never sent in the clear, whatever the answer says.
	//
	// Reporting it is still worth doing, since an application usually wants to tell the user why a
	// track went away. A provisional answer deserves care here: signalling carries the type out of
	// band, so the application knows whether it holds an answer or a pranswer, and a pranswer is
	// not the peer's final word.
	bool chainsBuilt = false;
	runSignalling(
	    pc,
	    [&](const rtc::Description &remote) {
		    for (int i = 0; i < remote.mediaCount(); ++i) {
			    // media(i) hands back a variant by value, so it is held in a local before being
			    // inspected.
			    auto entry = remote.media(i);
			    auto media = std::get_if<const rtc::Description::Media *>(&entry);
			    if (media && !(*media)->hasSFrame())
				    std::cout << std::endl
				              << "*** The peer did not accept SFrame on mid=\"" << (*media)->mid()
				              << "\", so that track is being stopped rather than sent in the "
				                 "clear."
				              << std::endl;
		    }
		    return true;
	    },
	    [&](Kind kind, const KeyMaterial &peer) {
		    // The peer's sending key for that m-line, used for decryption only, and refused
		    // outright if it collides with the key we send under.
		    if (kind == Kind::Video)
			    installPeerKey(videoReceiveProvider, videoSendKey, peer);
		    else
			    installPeerKey(audioReceiveProvider, audioSendKey, peer);
	    },
	    [&](const rtc::Description &applied) {
		    // The answer has been applied, so both unknowns are now settled and the chains can be
		    // built. Done once: a later renegotiation only needs this repeating if it changed
		    // something a chain depends on.
		    if (chainsBuilt)
			    return;
		    chainsBuilt = true;

		    const auto video = negotiatedMedia(applied, videoTrack->mid());
		    const auto audio = negotiatedMedia(applied, audioTrack->mid());

		    auto report = [](const char *label, const NegotiatedMedia &negotiated) {
			    if (!negotiated.found || negotiated.payloadType == 0) {
				    std::cout << "[" << label
				              << ": the answer named no codec, so nothing will be sent]"
				              << std::endl;
				    return;
			    }
			    std::cout << "[" << label << ": payload type " << unsigned(negotiated.payloadType)
			              << ", SFrame " << (negotiated.sframe ? "on" : "declined, track stopped")
			              << "]" << std::endl;
		    };
		    report("video", video);
		    report("audio", audio);

		    if (video.found && video.payloadType)
			    buildVideoChain(video.payloadType);
		    if (audio.found && audio.payloadType)
			    buildAudioChain(audio.payloadType);
	    });

	stop = true;
	sender.join();
	pc->close();
	return 0;
}
