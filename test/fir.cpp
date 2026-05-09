/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

static const SSRC SSRC_VIDEO = 1234;
static const SSRC SSRC_AUDIO = 5432;
static const uint8_t PT_VIDEO = 96;
static const uint8_t PT_AUDIO = 111;
static const uint32_t BITRATE = 3000;
static const uint16_t PORT_RANGE_BEGIN = 5000;
static const uint16_t PORT_RANGE_END = 6000;
static const char *CNAME_VIDEO = "camera";
static const char *CNAME_AUDIO = "mic";


// FIR SDP syntax tests
TestResult test_fir_sdp() {
	InitLogger(LogLevel::Debug);

	// Build a description with one video and one audio media
	Description desc("v=0\r\n"
	                  "o=- 0 0 IN IP4 0.0.0.0\r\n"
	                  "s=-\r\n"
	                  "t=0 0\r\n",
	                  Description::Type::Offer);

	// Start with just an audio track that does not have fir
	Description::Audio audio("audio", Description::Direction::SendOnly);
	audio.addOpusCodec(111);
	desc.addMedia(audio);

	string sdp1 = desc.generateSdp();
	if(sdp1.find("ccm fir") != string::npos) {
		return TestResult(false, "FIR entry found when not explicitly added");
	}

	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(96);
	video.addVP8Codec(97);
	desc.addMedia(video);

	sdp1 = desc.generateSdp();
	if(sdp1.find("ccm fir") == string::npos)
		return TestResult(false, "FIR entry not found in Offer");

	return TestResult(true);
}

// Negotiation tests: Test various combos of Offer / Answer

// FIR negotiation via PeerConnection — offer has it, answer does too
TestResult test_fir_offer_yes_answer_yes() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;
	PeerConnection pc2(config2);
	shared_ptr<Track> t2_video, t2_audio;

	promise<bool> recvPromisePliCalled;
	promise<bool> t2_video_OnTrackCalled;
	promise<bool> t2_audio_OnTrackCalled;

	pc1.onLocalDescription([&pc2](Description sdp) {
		cout << "Description 1: " << sdp << endl;
		pc2.setRemoteDescription(string(sdp));
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		cout << "Candidate 1: " << candidate << endl;
		pc2.addRemoteCandidate(string(candidate));
	});

	pc1.onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });

	pc1.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
	});

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		cout << "Candidate 2: " << candidate << endl;
		pc1.addRemoteCandidate(string(candidate));
	});

	pc2.onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });

	pc2.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	pc2.onTrack([&t2_video,&t2_audio,&t2_video_OnTrackCalled,&t2_audio_OnTrackCalled](shared_ptr<Track> t) {
		string mid = t->mid();

		cout << "Track 2: Received track with mid \"" << mid << "\"" << endl;

		t->onOpen([mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is open" << endl; });

		t->onClosed([mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is closed" << endl; });

		auto desc = t->description();

		if (mid == "video")
			desc.addSSRC(SSRC_VIDEO, CNAME_VIDEO);
		else
			desc.addSSRC(SSRC_AUDIO, CNAME_AUDIO);
		t->setDescription(desc);

		auto receivingSession = make_shared<RtcpReceivingSession>();
		t->setMediaHandler(receivingSession);

		if (mid == "video") {
			std::atomic_store(&t2_video, t);
			t2_video_OnTrackCalled.set_value(true);
		} else {
			std::atomic_store(&t2_audio, t);
			t2_audio_OnTrackCalled.set_value(true);
		}
	});
	Description::Video video("video", Description::Direction::SendOnly);
	video.addH264Codec(PT_VIDEO);
	video.setBitrate(BITRATE);
	video.addSSRC(SSRC_VIDEO, CNAME_VIDEO);

	auto t1_video = pc1.addTrack(video);
	auto rtpConfigVideo =
	    make_shared<RtpPacketizationConfig>(SSRC_VIDEO, CNAME_VIDEO, PT_VIDEO, 90000);

	auto srReporter = make_shared<RtcpSrReporter>(rtpConfigVideo);
	auto nackResponder = make_shared<RtcpNackResponder>();
	auto pliHandler = make_shared<PliHandler>([&recvPromisePliCalled](void) {
		cout << "PLI Handler Called" << endl;
		recvPromisePliCalled.set_value(true);
	});

	nackResponder->addToChain(pliHandler);
	srReporter->addToChain(nackResponder);
	t1_video->setMediaHandler(srReporter);

	Description::Audio audio("audio", Description::Direction::SendRecv);
	audio.addOpusCodec(111);
	audio.addSSRC(SSRC_AUDIO, "audio-send");
	auto t1_audio = pc1.addTrack(audio);
	auto rtpConfigAudio =
	    make_shared<RtpPacketizationConfig>(SSRC_AUDIO, CNAME_AUDIO, PT_AUDIO, 48000);

	t1_audio->setMediaHandler(make_shared<RtcpSrReporter>(rtpConfigAudio));

	pc1.setLocalDescription();

	// Wait for connection
	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2_video)) || !at2->isOpen() || !t1_video->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected || pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	// Wait for the remote track to get set
	if (t2_video_OnTrackCalled.get_future().wait_for(5s) == future_status::timeout)
		return TestResult(false, "Track 2: never got set");

	// Try to request a key frame from the remote audio track and make sure it fails
	if (t2_audio->requestKeyframe(SSRC_AUDIO))
		return TestResult(false, "Should not be able to request key frame on audio track");

	// Send a default FIR request from the remote track and make sure it arrives.
	// Testing using the default constructor fails in this limited unit test since track level ssrc is not set without media.
	//if (!t2_video->requestKeyframe())
	//	return TestResult(false, "Unable to request default FIR on pc2 t2 track");
	//if (recvPromisePliCalled.get_future().wait_for(5s) == future_status::timeout)
	//	return TestResult(false, "Didn't receive default FIR on pc1 PLI Handler");

	// Send a target FIR from the remote track and make sure it arrives.
	recvPromisePliCalled = promise<bool>();
	if (!t2_video->requestKeyframe(SSRC_VIDEO))
		return TestResult(false, "Unable to request targeted FIR on pc2 t2 track");
	if (recvPromisePliCalled.get_future().wait_for(5s) == future_status::timeout)
		return TestResult(false, "Didn't receive single target FIR on pc1 PLI Handler");

	// Request three retransmits and make sure it does not invoke callback
	recvPromisePliCalled = promise<bool>();
	if (!t2_video->requestKeyframe(std::vector<SSRC>(3,SSRC_VIDEO), true))
		return TestResult(false, "Unable to request retransmitted FIRs on pc2 t2 track");
	if (recvPromisePliCalled.get_future().wait_for(1s) != future_status::timeout)
		return TestResult(false, "Retransmits caused pli handler to be called");

	// Send a multi fci FIR and make sure it arrives.
	recvPromisePliCalled = promise<bool>();
	if (!t2_video->requestKeyframe(std::vector<SSRC>(3,SSRC_VIDEO)))
		return TestResult(false, "Unable to request second FIR on pc2 t2 track");
	if (recvPromisePliCalled.get_future().wait_for(5s) == future_status::timeout)
		return TestResult(false, "Didn't receive second FIR on pc1 PLI Handler");

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}

#endif // RTC_ENABLE_MEDIA
