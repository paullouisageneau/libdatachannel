/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

void test_track() {
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
	pc2.onTrack([&t2, &newTrackMid](shared_ptr<Track> t) {
		string mid = t->mid();
		cout << "Track 2: Received track with mid \"" << mid << "\"" << endl;
		if (mid != newTrackMid) {
			cerr << "Wrong track mid" << endl;
			return;
		}

		t->onOpen([mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is open" << endl; });

		t->onClosed(
		    [mid]() { cout << "Track 2: Track with mid \"" << mid << "\" is closed" << endl; });

		std::atomic_store(&t2, t);
	});

	// Test opening a track
	newTrackMid = "test";

	Description::Video media(newTrackMid, Description::Direction::SendOnly);
	media.addH264Codec(96);
	media.setBitrate(3000);
	media.addSSRC(1234, "video-send");

	auto t1 = pc1.addTrack(media);

	pc1.setLocalDescription();

	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if (!at2 || !at2->isOpen() || !t1->isOpen())
		throw runtime_error("Track is not open");

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
		throw runtime_error("Renegotiated track is not open");

	// TODO: Test sending RTP packets in track

	// Delay close of peer 2 to check closing works properly
	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	if (!t1->isClosed() || !t2->isClosed())
		throw runtime_error("Track is not closed");

	cout << "Success" << endl;
}
