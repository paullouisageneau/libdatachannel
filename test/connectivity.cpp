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

#define CUSTOM_MAX_MESSAGE_SIZE 1048576

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

void test_connectivity() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	// STUN server example (not necessary to connect locally)
	// Please do not use outside of libdatachannel tests
	config1.iceServers.emplace_back("stun:stun.ageneau.net:3478");
	// Custom MTU example
	config1.mtu = 1500;
	// Custom max message size
	config1.maxMessageSize = CUSTOM_MAX_MESSAGE_SIZE;

	PeerConnection pc1(config1);

	Configuration config2;
	// STUN server example (not necessary to connect locally)
	// Please do not use outside of libdatachannel tests
	config2.iceServers.emplace_back("stun:stun.ageneau.net:3478");
	// Custom MTU example
	config2.mtu = 1500;
	// Custom max message size
	config2.maxMessageSize = CUSTOM_MAX_MESSAGE_SIZE;
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

	pc1.onSignalingStateChange([](PeerConnection::SignalingState state) {
		cout << "Signaling state 1: " << state << endl;
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
		if (auto dc1 = wdc1.lock()) {
			cout << "DataChannel 1: Open" << endl;
			dc1->send("Hello from 1");
		}
	});

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

	if (pc1.state() != PeerConnection::State::Connected &&
	    pc2.state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if (!adc2 || !adc2->isOpen() || !dc1->isOpen())
		throw runtime_error("DataChannel is not open");

	if (dc1->maxMessageSize() != CUSTOM_MAX_MESSAGE_SIZE ||
	    dc2->maxMessageSize() != CUSTOM_MAX_MESSAGE_SIZE)
		throw runtime_error("DataChannel max message size is incorrect");

	if (auto addr = pc1.localAddress())
		cout << "Local address 1:  " << *addr << endl;
	if (auto addr = pc1.remoteAddress())
		cout << "Remote address 1: " << *addr << endl;
	if (auto addr = pc2.localAddress())
		cout << "Local address 2:  " << *addr << endl;
	if (auto addr = pc2.remoteAddress())
		cout << "Remote address 2: " << *addr << endl;

	Candidate local, remote;
	if (pc1.getSelectedCandidatePair(&local, &remote)) {
		cout << "Local candidate 1:  " << local << endl;
		cout << "Remote candidate 1: " << remote << endl;
	}
	if (pc2.getSelectedCandidatePair(&local, &remote)) {
		cout << "Local candidate 2:  " << local << endl;
		cout << "Remote candidate 2: " << remote << endl;
	}

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
	second1->onOpen([wsecond1 = make_weak_ptr(dc1)]() {
		if (auto second1 = wsecond1.lock()) {
			cout << "Second DataChannel 1: Open" << endl;
			second1->send("Second hello from 1");
		}
	});
	dc1->onMessage([](const variant<binary, string> &message) {
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
