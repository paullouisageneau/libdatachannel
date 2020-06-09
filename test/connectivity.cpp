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

void test_connectivity() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	// STUN server example
	// config1.iceServers.emplace_back("stun:stun.l.google.com:19302");

	auto pc1 = std::make_shared<PeerConnection>(config1);

	Configuration config2;
	// STUN server example
	// config2.iceServers.emplace_back("stun:stun.l.google.com:19302");
	// Port range example
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;

	auto pc2 = std::make_shared<PeerConnection>(config2);

	pc1->onLocalDescription([wpc2 = make_weak_ptr(pc2)](const Description &sdp) {
		auto pc2 = wpc2.lock();
		if (!pc2)
			return;
		cout << "Description 1: " << sdp << endl;
		pc2->setRemoteDescription(sdp);
	});

	pc1->onLocalCandidate([wpc2 = make_weak_ptr(pc2)](const Candidate &candidate) {
		auto pc2 = wpc2.lock();
		if (!pc2)
			return;
		cout << "Candidate 1: " << candidate << endl;
		pc2->addRemoteCandidate(candidate);
	});

	pc1->onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });

	pc1->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
	});

	pc2->onLocalDescription([wpc1 = make_weak_ptr(pc1)](const Description &sdp) {
		auto pc1 = wpc1.lock();
		if (!pc1)
			return;
		cout << "Description 2: " << sdp << endl;
		pc1->setRemoteDescription(sdp);
	});

	pc2->onLocalCandidate([wpc1 = make_weak_ptr(pc1)](const Candidate &candidate) {
		auto pc1 = wpc1.lock();
		if (!pc1)
			return;
		cout << "Candidate 2: " << candidate << endl;
		pc1->addRemoteCandidate(candidate);
	});

	pc2->onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });

	pc2->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	shared_ptr<DataChannel> dc2;
	pc2->onDataChannel([&dc2](shared_ptr<DataChannel> dc) {
		cout << "DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;

		dc->onMessage([](const variant<binary, string> &message) {
			if (holds_alternative<string>(message)) {
				cout << "Message 2: " << get<string>(message) << endl;
			}
		});

		dc->send("Hello from 2");

		std::atomic_store(&dc2, dc);
	});

	auto dc1 = pc1->createDataChannel("test");
	dc1->onOpen([wdc1 = make_weak_ptr(dc1)]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		cout << "DataChannel 1: Open" << endl;
		dc1->send("Hello from 1");
	});
	dc1->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Message 1: " << get<string>(message) << endl;
		}
	});

	int attempts = 10;
	shared_ptr<DataChannel> adc2;
	while ((!(adc2 = std::atomic_load(&dc2)) || !adc2->isOpen() || !dc1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1->state() != PeerConnection::State::Connected &&
	    pc2->state() != PeerConnection::State::Connected)
		throw runtime_error("PeerConnection is not connected");

	if (!adc2 || !adc2->isOpen() || !dc1->isOpen())
		throw runtime_error("DataChannel is not open");

	if (auto addr = pc1->localAddress())
		cout << "Local address 1:  " << *addr << endl;
	if (auto addr = pc1->remoteAddress())
		cout << "Remote address 1: " << *addr << endl;
	if (auto addr = pc2->localAddress())
		cout << "Local address 2:  " << *addr << endl;
	if (auto addr = pc2->remoteAddress())
		cout << "Remote address 2: " << *addr << endl;

	// Delay close of peer 2 to check closing works properly
	pc1->close();
	this_thread::sleep_for(1s);
	pc2->close();
	this_thread::sleep_for(1s);

	// You may call rtc::Cleanup() when finished to free static resources
	rtc::Cleanup();
	this_thread::sleep_for(1s);

	cout << "Success" << endl;
}
