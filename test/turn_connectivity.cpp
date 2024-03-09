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

void test_turn_connectivity() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	config1.iceTransportPolicy = TransportPolicy::Relay; // force relay

	// TURN server example (use your own server in production)
	config1.iceServers.emplace_back(
	    "turn:openrelayproject:openrelayproject@openrelay.metered.ca:80");

	PeerConnection pc1(config1);

	Configuration config2;

	// STUN server example (use your own server in production)
	config2.iceServers.emplace_back("stun:openrelay.metered.ca:80");

	PeerConnection pc2(config2);

	pc1.onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });

	pc1.onIceStateChange(
	    [](PeerConnection::IceState state) { cout << "ICE state 1: " << state << endl; });

	pc1.onGatheringStateChange([&pc1, &pc2](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
		if (state == PeerConnection::GatheringState::Complete) {
			auto sdp = pc1.localDescription().value();
			cout << "Description 1: " << sdp << endl;
			pc2.setRemoteDescription(string(sdp));
		}
	});

	pc1.onSignalingStateChange([](PeerConnection::SignalingState state) {
		cout << "Signaling state 1: " << state << endl;
	});

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		// Filter server reflexive candidates
		if (candidate.type() != rtc::Candidate::Type::ServerReflexive)
			return;

		cout << "Candidate 2: " << candidate << endl;
		pc1.addRemoteCandidate(string(candidate));
	});

	pc2.onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });

	pc2.onIceStateChange(
	    [](PeerConnection::IceState state) { cout << "ICE state 2: " << state << endl; });

	pc2.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	pc2.onSignalingStateChange([](PeerConnection::SignalingState state) {
		cout << "Signaling state 2: " << state << endl;
	});

	shared_ptr<DataChannel> dc2;
	pc2.onDataChannel([&dc2](shared_ptr<DataChannel> dc) {
		cout << "DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;
		if (dc->label() != "test") {
			cerr << "Wrong DataChannel label" << endl;
			return;
		}

		dc->onOpen([wdc = make_weak_ptr(dc)]() {
			if (auto dc = wdc.lock()) {
				cout << "DataChannel 2: Open" << endl;
				dc->send("Hello from 2");
			}
		});

		dc->onMessage([](variant<binary, string> message) {
			if (holds_alternative<string>(message)) {
				cout << "Message 2: " << get<string>(message) << endl;
			}
		});

		std::atomic_store(&dc2, dc);
	});

	auto dc1 = pc1.createDataChannel("test");
	dc1->onOpen([wdc1 = make_weak_ptr(dc1)]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		cout << "DataChannel 1: Open" << endl;
		dc1->send("Hello from 1");
	});

	dc1->onClosed([]() { cout << "DataChannel 1: Closed" << endl; });

	dc1->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Message 1: " << get<string>(message) << endl;
		}
	});

	// Wait a bit
	int attempts = 10;
	shared_ptr<DataChannel> adc2;
	while ((!(adc2 = std::atomic_load(&dc2)) || !adc2->isOpen() || !dc1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if ((pc1.iceState() != PeerConnection::IceState::Connected &&
	     pc1.iceState() != PeerConnection::IceState::Completed) ||
	    (pc2.iceState() != PeerConnection::IceState::Connected &&
	     pc2.iceState() != PeerConnection::IceState::Completed))
		throw runtime_error("ICE is not connected");

	if (!adc2 || !adc2->isOpen() || !dc1->isOpen())
		throw runtime_error("DataChannel is not open");

	if (auto addr = pc1.localAddress())
		cout << "Local address 1:  " << *addr << endl;
	if (auto addr = pc1.remoteAddress())
		cout << "Remote address 1: " << *addr << endl;
	if (auto addr = pc2.localAddress())
		cout << "Local address 2:  " << *addr << endl;
	if (auto addr = pc2.remoteAddress())
		cout << "Remote address 2: " << *addr << endl;

	Candidate local, remote;
	if (!pc1.getSelectedCandidatePair(&local, &remote))
		throw runtime_error("getSelectedCandidatePair failed");

	cout << "Local candidate 1:  " << local << endl;
	cout << "Remote candidate 1: " << remote << endl;

	if (local.type() != Candidate::Type::Relayed)
		throw runtime_error("Connection is not relayed as expected");

	// Try to open a second data channel with another label
	shared_ptr<DataChannel> second2;
	pc2.onDataChannel([&second2](shared_ptr<DataChannel> dc) {
		cout << "Second DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;
		if (dc->label() != "second") {
			cerr << "Wrong second DataChannel label" << endl;
			return;
		}

		dc->onOpen([wdc = make_weak_ptr(dc)]() {
			if (auto dc = wdc.lock())
				dc->send("Second hello from 2");
		});

		dc->onMessage([](variant<binary, string> message) {
			if (holds_alternative<string>(message)) {
				cout << "Second Message 2: " << get<string>(message) << endl;
			}
		});

		std::atomic_store(&second2, dc);
	});

	auto second1 = pc1.createDataChannel("second");

	second1->onOpen([wsecond1 = make_weak_ptr(second1)]() {
		auto second1 = wsecond1.lock();
		if (!second1)
			return;

		cout << "Second DataChannel 1: Open" << endl;
		second1->send("Second hello from 1");
	});

	second1->onClosed([]() { cout << "Second DataChannel 1: Closed" << endl; });

	second1->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Second Message 1: " << get<string>(message) << endl;
		}
	});

	// Wait a bit
	attempts = 10;
	shared_ptr<DataChannel> asecond2;
	while (
	    (!(asecond2 = std::atomic_load(&second2)) || !asecond2->isOpen() || !second1->isOpen()) &&
	    attempts--)
		this_thread::sleep_for(1s);

	if (!asecond2 || !asecond2->isOpen() || !second1->isOpen())
		throw runtime_error("Second DataChannel is not open");

	// Try to open a negotiated channel
	DataChannelInit init;
	init.negotiated = true;
	init.id = 42;
	auto negotiated1 = pc1.createDataChannel("negotiated", init);
	auto negotiated2 = pc2.createDataChannel("negoctated", init);

	if (!negotiated1->isOpen() || !negotiated2->isOpen())
		throw runtime_error("Negotiated DataChannel is not open");

	std::atomic<bool> received = false;
	negotiated2->onMessage([&received](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Second Message 2: " << get<string>(message) << endl;
			received = true;
		}
	});

	negotiated1->send("Hello from negotiated channel");

	// Wait a bit
	attempts = 5;
	while (!received && attempts--)
		this_thread::sleep_for(1s);

	if (!received)
		throw runtime_error("Negotiated DataChannel failed");

	// Delay close of peer 2 to check closing works properly
	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	cout << "Success" << endl;
}
