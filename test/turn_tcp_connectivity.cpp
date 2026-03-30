/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

TestResult test_turn_tcp_connectivity() {
	// Allow skipping this test if no TURN server is available
	const char *skip = getenv("SKIP_TURN_TCP_TEST");
	if (skip && string(skip) != "0") {
		cout << "TURN TCP connectivity test skipped (SKIP_TURN_TCP_TEST is set)" << endl;
		return TestResult(true);
	}

	// Read TURN server configuration from environment variables.
	// If not set, attempt to connect to a local coturn instance on 127.0.0.1:3478.
	const char *turn_host_env = getenv("TURN_HOST");
	const char *turn_port_env = getenv("TURN_PORT");
	const char *turn_user_env = getenv("TURN_USERNAME");
	const char *turn_pass_env = getenv("TURN_PASSWORD");

	if (!turn_host_env || !turn_port_env || !turn_user_env || !turn_pass_env) {
		cout << "TURN TCP connectivity test skipped (missing environment)" << endl;
		return TestResult(true);
	}
	string turn_host = turn_host_env ? turn_host_env : "127.0.0.1";
	string turn_port = turn_port_env ? turn_port_env : "3478";
	string turn_user = turn_user_env ? turn_user_env : "testuser";
	string turn_pass = turn_pass_env ? turn_pass_env : "testpassword";

	// Build TURN URI: turn:<user>:<pass>@<host>:<port>?transport=tcp
	// The IceServer constructor also accepts a relayType parameter.
	IceServer turnServer(turn_host, (uint16_t)stoi(turn_port), turn_user, turn_pass,
	                     IceServer::RelayType::TurnTcp);

	InitLogger(LogLevel::Debug);

	Configuration config1;
	config1.iceTransportPolicy = TransportPolicy::Relay; // force relay
	config1.iceServers.push_back(turnServer);

	PeerConnection pc1(config1);

	Configuration config2;
	config2.iceTransportPolicy = TransportPolicy::Relay; // force relay
	config2.iceServers.push_back(turnServer);

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
		// Only forward relay candidates
		if (candidate.type() != rtc::Candidate::Type::Relayed)
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

	const int NUM_PACKETS = 20;
	const int MIN_SIZE = 100;
	const int MAX_SIZE = 2048;

	atomic<int> dc1_recv_count(0);
	atomic<int> dc2_recv_count(0);
	atomic<size_t> dc1_recv_bytes(0);
	atomic<size_t> dc2_recv_bytes(0);

	shared_ptr<DataChannel> dc2;
	pc2.onDataChannel([&](shared_ptr<DataChannel> dc) {
		cout << "DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;
		if (dc->label() != "test") {
			cerr << "Wrong DataChannel label" << endl;
			return;
		}
		dc->onOpen([wdc = make_weak_ptr(dc)]() {
			if (auto dc = wdc.lock())
				cout << "DataChannel 2: Open" << endl;
		});
		dc->onMessage([&](variant<binary, string> message) {
			if (holds_alternative<binary>(message)) {
				dc2_recv_bytes += get<binary>(message).size();
				int n = ++dc2_recv_count;
				cout << "DC2 recv packet " << n << " (" << get<binary>(message).size() << " bytes)" << endl;
			}
		});
		std::atomic_store(&dc2, dc);
	});

	auto dc1 = pc1.createDataChannel("test");
	dc1->onOpen([wdc1 = make_weak_ptr(dc1)]() {
		if (auto dc1 = wdc1.lock())
			cout << "DataChannel 1: Open" << endl;
	});
	dc1->onClosed([]() { cout << "DataChannel 1: Closed" << endl; });
	dc1->onMessage([&](const variant<binary, string> &message) {
		if (holds_alternative<binary>(message)) {
			dc1_recv_bytes += get<binary>(message).size();
			int n = ++dc1_recv_count;
			cout << "DC1 recv packet " << n << " (" << get<binary>(message).size() << " bytes)" << endl;
		}
	});

	// Wait up to 45s for connection (ICE PAC timeout is 39.5s, need margin for TURN server unreachable case)
	int attempts = 45;
	shared_ptr<DataChannel> adc2;
	while ((!(adc2 = std::atomic_load(&dc2)) || !adc2->isOpen() || !dc1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	// If not connected, the TURN server is probably not available – skip gracefully
	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected) {
		pc1.close();
		pc2.close();
		return TestResult(false, "PeerConnection is not connected");
	}

	if ((pc1.iceState() != PeerConnection::IceState::Connected &&
	     pc1.iceState() != PeerConnection::IceState::Completed) ||
	    (pc2.iceState() != PeerConnection::IceState::Connected &&
	     pc2.iceState() != PeerConnection::IceState::Completed))
		return TestResult(false, "ICE is not connected");

	if (!adc2 || !adc2->isOpen() || !dc1->isOpen())
		return TestResult(false, "DataChannel is not open");

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
		return TestResult(false, "getSelectedCandidatePair failed");

	cout << "Local candidate 1:  " << local << endl;
	cout << "Remote candidate 1: " << remote << endl;

	if (local.type() != Candidate::Type::Relayed)
		return TestResult(false, "Connection is not relayed as expected");

	// Send 20 packets of random size (100-2048 bytes) in each direction
	mt19937 rng(42); // fixed seed for reproducibility
	uniform_int_distribution<int> size_dist(MIN_SIZE, MAX_SIZE);

	size_t total_sent_1to2 = 0;
	size_t total_sent_2to1 = 0;

	cout << "Sending " << NUM_PACKETS << " packets in each direction (size " << MIN_SIZE << "-" << MAX_SIZE << " bytes)" << endl;

	for (int i = 0; i < NUM_PACKETS; i++) {
		int pkt_size_1 = size_dist(rng);
		int pkt_size_2 = size_dist(rng);

		binary data1(pkt_size_1);
		binary data2(pkt_size_2);
		for (int j = 0; j < pkt_size_1; j++) data1[j] = byte(rng() & 0xFF);
		for (int j = 0; j < pkt_size_2; j++) data2[j] = byte(rng() & 0xFF);

		cout << "Send packet " << (i + 1) << ": DC1->" << pkt_size_1 << "B, DC2->" << pkt_size_2 << "B" << endl;
		dc1->send(data1);
		adc2->send(data2);
		total_sent_1to2 += pkt_size_1;
		total_sent_2to1 += pkt_size_2;		
	}

	// Wait for all packets to arrive (up to 10s)
	attempts = 100;
	while ((dc1_recv_count < NUM_PACKETS || dc2_recv_count < NUM_PACKETS) && attempts--)
		this_thread::sleep_for(100ms);

	cout << "Results: DC1 sent " << total_sent_1to2 << " bytes in " << NUM_PACKETS << " packets" << endl;
	cout << "Results: DC2 sent " << total_sent_2to1 << " bytes in " << NUM_PACKETS << " packets" << endl;
	cout << "Results: DC1 received " << dc1_recv_count << "/" << NUM_PACKETS
	     << " packets (" << dc1_recv_bytes << " bytes)" << endl;
	cout << "Results: DC2 received " << dc2_recv_count << "/" << NUM_PACKETS
	     << " packets (" << dc2_recv_bytes << " bytes)" << endl;

	if (dc2_recv_count != NUM_PACKETS)
		return TestResult(false, "DC2 did not receive all packets (got " +
		                  to_string(dc2_recv_count.load()) + "/" + to_string(NUM_PACKETS) + ")");

	if (dc1_recv_count != NUM_PACKETS)
		return TestResult(false, "DC1 did not receive all packets (got " +
		                  to_string(dc1_recv_count.load()) + "/" + to_string(NUM_PACKETS) + ")");

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}
