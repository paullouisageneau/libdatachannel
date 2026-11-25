/**
 * Copyright (c) 2026 mertushka
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace rtc;
using namespace std;

TestResult test_send_batch() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	config1.disableAutoNegotiation = true;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.disableAutoNegotiation = true;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		pc2.setRemoteDescription(string(sdp));
		pc2.setLocalDescription(); // Make the answer
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		pc2.addRemoteCandidate(string(candidate));
	});

	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		pc1.addRemoteCandidate(string(candidate));
	});

	DataChannelInit init;
	init.negotiated = true;
	init.id = 1;
	auto dc1 = pc1.createDataChannel("batch", init);
	auto dc2 = pc2.createDataChannel("batch", init);

	std::mutex mutex;
	vector<message_variant> received;
	dc2->onMessage([&](message_variant message) {
		std::lock_guard lock(mutex);
		received.push_back(std::move(message));
	});

	pc1.setLocalDescription();

	int attempts = 10;
	while ((!dc1->isOpen() || !dc2->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (!dc1->isOpen() || !dc2->isOpen())
		return TestResult(false, "DataChannel is not open");

	// Mixed strings, binary, and empty messages must arrive complete and in order
	vector<message_variant> expected;
	for (int i = 0; i < 1000; ++i) {
		if (i % 3 == 0)
			expected.emplace_back(binary(size_t(i % 17), byte(i & 0xFF)));
		else
			expected.emplace_back("message " + to_string(i));
	}
	dc1->send(expected);

	// A single-message batch behaves like send()
	expected.emplace_back(string("last"));
	dc1->send(vector<message_variant>{string("last")});

	// An oversized message rejects the whole batch before anything is sent
	bool rejected = false;
	try {
		dc1->send(vector<message_variant>{string("never sent"), binary(dc1->maxMessageSize() + 1)});
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	if (!rejected)
		return TestResult(false, "Oversized batch was not rejected");

	attempts = 10;
	while (attempts--) {
		{
			std::lock_guard lock(mutex);
			if (received.size() >= expected.size())
				break;
		}
		this_thread::sleep_for(1s);
	}

	{
		std::lock_guard lock(mutex);
		if (received.size() != expected.size())
			return TestResult(false, "Expected " + to_string(expected.size()) + " messages, got " +
			                             to_string(received.size()));
		if (received != expected)
			return TestResult(false, "Batched messages arrived out of order or modified");
	}

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}
