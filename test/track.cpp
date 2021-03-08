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
		cout << "Track 2: Received with mid \"" << t->mid() << "\"" << endl;
		if (t->mid() != newTrackMid) {
			cerr << "Wrong track mid" << endl;
			return;
		}

		std::atomic_store(&t2, t);
	});

	// Test opening a track
	newTrackMid = "test";
	auto t1 = pc1.addTrack(Description::Video(newTrackMid));

	pc1.setLocalDescription();

	int attempts = 10;
	shared_ptr<Track> at2;
	while ((!(at2 = std::atomic_load(&t2)) || !at2->isOpen() || !t1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected &&
	    pc2.state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if (!at2 || !at2->isOpen() || !t1->isOpen())
		throw runtime_error("Track is not open");

	// Test renegotiation
	newTrackMid = "added";
	t1 = pc1.addTrack(Description::Video(newTrackMid));

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

	cout << "Success" << endl;
}
