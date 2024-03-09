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

void test_reliability() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	// STUN server example (not necessary to connect locally)
	config1.iceServers.emplace_back("stun:stun.l.google.com:19302");

	PeerConnection pc1(config1);

	Configuration config2;
	// STUN server example (not necessary to connect locally)
	config2.iceServers.emplace_back("stun:stun.l.google.com:19302");

	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		cout << "Description 1: " << sdp << endl;
		pc2.setRemoteDescription(string(sdp));
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

	Reliability reliableOrdered;
	auto dcReliableOrdered = pc1.createDataChannel("reliable_ordered", {reliableOrdered});

	Reliability reliableUnordered;
	reliableUnordered.unordered = true;
	auto dcReliableUnordered = pc1.createDataChannel("reliable_unordered", {reliableUnordered});

	Reliability unreliableMaxPacketLifeTime;
	unreliableMaxPacketLifeTime.unordered = true;
	unreliableMaxPacketLifeTime.maxPacketLifeTime = 222ms;
	auto dcUnreliableMaxPacketLifeTime =
	    pc1.createDataChannel("unreliable_maxpacketlifetime", {unreliableMaxPacketLifeTime});

	Reliability unreliableMaxRetransmits;
	unreliableMaxRetransmits.unordered = true;
	unreliableMaxRetransmits.maxRetransmits = 2;
	auto dcUnreliableMaxRetransmits =
	    pc1.createDataChannel("unreliable_maxretransmits", {unreliableMaxRetransmits});

	std::atomic<int> count = 0;
	std::atomic<bool> failed = false;
	pc2.onDataChannel([&count, &failed](shared_ptr<DataChannel> dc) {
		cout << "DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;

		auto label = dc->label();
		auto reliability = dc->reliability();

		try {
			if (label == "reliable_ordered") {
				if (reliability.unordered != false || reliability.maxPacketLifeTime ||
				    reliability.maxRetransmits)
					throw std::runtime_error("Expected reliable ordered");
			} else if (label == "reliable_unordered") {
				if (reliability.unordered != true || reliability.maxPacketLifeTime ||
				    reliability.maxRetransmits)
					throw std::runtime_error("Expected reliable unordered");
			} else if (label == "unreliable_maxpacketlifetime") {
				if (!reliability.maxPacketLifeTime || *reliability.maxPacketLifeTime != 222ms ||
				    reliability.maxRetransmits)
					throw std::runtime_error("Expected maxPacketLifeTime to be set");
			} else if (label == "unreliable_maxretransmits") {
				if (reliability.maxPacketLifeTime || !reliability.maxRetransmits ||
				    *reliability.maxRetransmits != 2)
					throw std::runtime_error("Expected maxRetransmits to be set");
			} else
				throw std::runtime_error("Unexpected label: " + label);
		} catch (const std::exception &e) {
			cerr << "Error: " << e.what();
			failed = true;
			return;
		}
		++count;
	});

	// Wait a bit
	int attempts = 10;
	shared_ptr<DataChannel> adc2;
	while (count != 4 && !failed && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if (failed)
		throw runtime_error("Incorrect reliability settings");

	if (count != 4)
		throw runtime_error("Some DataChannels are not open");

	pc1.close();

	cout << "Success" << endl;
}
