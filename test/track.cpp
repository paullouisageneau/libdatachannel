/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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
#include <future>
#include <iostream>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

TestResult test_track() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	// STUN server example
	// config1.iceServers.emplace_back("stun:stun.l.google.com:19302");

	PeerConnection pc1(config1);

	Configuration config2;
	// STUN server example
	// config2.iceServers.emplace_back("stun:stun.l.google.com:19302");
	// Port range example
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;

	PeerConnection pc2(config2);

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

	shared_ptr<Track> t2;
	string newTrackMid;
	std::promise<rtc::binary> recvRtpPromise;
	pc2.onTrack([&t2, &newTrackMid, &recvRtpPromise](shared_ptr<Track> t) {
		string mid = t->mid();
		cout << "Track 2: Received track with mid \"" << mid << "\"" << endl;
		if (mid != newTrackMid) {
			cerr << "Wrong track mid" << endl;
			return;
		}

		t->onOpen([mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is open" << endl; });

		t->onClosed(
		    [mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is closed" << endl; });

		t->onMessage(
		    [&recvRtpPromise](rtc::binary message) {
			    // This is an RTP packet
			    recvRtpPromise.set_value(message);
		    },
		    nullptr);

		std::atomic_store(&t2, t);
	});

	// Test opening a track
	newTrackMid = "test";

	Description::Video media(newTrackMid, Description::Direction::SendOnly);
	media.addH264Codec(96);
	media.setBitrate(3000);
	media.addSSRC(1234, "video-send");

	const auto mediaSdp1 = string(media);
	const auto mediaSdp2 = string(Description::Media(mediaSdp1));
	if (mediaSdp2 != mediaSdp1) {
		cout << mediaSdp2 << endl;
		return TestResult(false, "Media description parsing test failed");
	}

	auto t1 = pc1.addTrack(media);

	pc1.setLocalDescription();

	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	if (!at2 || !at2->isOpen() || !t1->isOpen())
		return TestResult(false, "Track is not open");

	// Test renegotiation
	newTrackMid = "added";

	Description::Video media2(newTrackMid, Description::Direction::SendOnly);
	media2.addH264Codec(96);
	media2.setBitrate(3000);
	media2.addSSRC(2468, "video-send");

	// NOTE: Overwriting the old shared_ptr for t1 will cause it's respective
	//       track to be dropped (so it's SSRCs won't be on the description next time)
	t1 = pc1.addTrack(media2);

	pc1.setLocalDescription();

	attempts = 10;
	t2.reset();
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (!at2 || !at2->isOpen() || !t1->isOpen())
		return TestResult(false, "Renegotiated track is not open");

	std::vector<std::byte> payload = {std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}};
	std::vector<std::byte> rtpRaw(sizeof(RtpHeader) + payload.size());
	auto *rtp = reinterpret_cast<RtpHeader *>(rtpRaw.data());
	rtp->setPayloadType(96);
	rtp->setSeqNumber(1);
	rtp->setTimestamp(3000);
	rtp->setSsrc(2468);
	rtp->preparePacket();
	std::memcpy(rtpRaw.data() + sizeof(RtpHeader), payload.data(), payload.size());

	if (!t1->send(rtpRaw.data(), rtpRaw.size())) {
		throw runtime_error("Couldn't send RTP packet");
	}

	// wait for an RTP packet to be received
	auto future = recvRtpPromise.get_future();
	if (future.wait_for(5s) == std::future_status::timeout) {
		throw runtime_error("Didn't receive RTP packet on pc2");
	}

	auto receivedRtpRaw = future.get();
	if (receivedRtpRaw.empty()) {
		throw runtime_error("Didn't receive RTP packet on pc2");
	}

	if (receivedRtpRaw.size() != rtpRaw.size() ||
	    memcmp(receivedRtpRaw.data(), rtpRaw.data(), rtpRaw.size()) != 0) {
		throw runtime_error("Received RTP packet is different than the packet that was sent");
	}

	// Delay close of peer 2 to check closing works properly
	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	if (!t1->isClosed() || !t2->isClosed())
		return TestResult(false, "Track is not closed");

	return TestResult(true);
}
