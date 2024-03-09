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

void test_negotiated() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	config1.disableAutoNegotiation = true;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.disableAutoNegotiation = true;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		cout << "Description 1: " << sdp << endl;
		pc2.setRemoteDescription(string(sdp));
		pc2.setLocalDescription(); // Make the answer
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		cout << "Candidate 1: " << candidate << endl;
		pc2.addRemoteCandidate(string(candidate));
	});

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		cout << "Candidate 2: " << candidate << endl;
		pc1.addRemoteCandidate(string(candidate));
	});

	// Try to open a negotiated channel
	DataChannelInit init;
	init.negotiated = true;
	init.id = 1;
	auto negotiated1 = pc1.createDataChannel("negotiated", init);
	auto negotiated2 = pc2.createDataChannel("negotiated", init);

	// Make the offer
	pc1.setLocalDescription();

	// Wait a bit
	int attempts = 10;
	while (!negotiated1->isOpen() || !negotiated2->isOpen() && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if (!negotiated1->isOpen() || !negotiated2->isOpen())
		throw runtime_error("Negotiated DataChannel is not open");

	std::atomic<bool> received = false;
	negotiated2->onMessage([&received](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Message 2: " << get<string>(message) << endl;
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
