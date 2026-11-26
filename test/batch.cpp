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

	// Declared before the peers so it outlives any late callback
	std::mutex mutex;
	vector<message_variant> received;
	std::atomic<int> lowCount = 0;

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

	// An empty batch sends nothing, so nothing is buffered
	if (!dc1->send(vector<message_variant>{}))
		return TestResult(false, "Empty batch was reported as buffered");

	// An oversized message rejects the whole batch before anything is sent
	bool rejected = false;
	try {
		dc1->send(vector<message_variant>{string("never sent"), binary(dc1->maxMessageSize() + 1)});
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	if (!rejected)
		return TestResult(false, "Oversized batch was not rejected");

	// Sizes around packet boundaries, including empty and fragmented messages, must not affect
	// the order
	const vector<size_t> sizes = {0,   1,   3,    4,    5,    64,   255,  256,  564,  565,
	                              568, 569, 1000, 1155, 1156, 1157, 1172, 2400, 16384};
	vector<message_variant> boundaries;
	for (size_t i = 0; i < 300; ++i) {
		const size_t size = sizes[(i * 7) % sizes.size()];
		if (i % 2)
			boundaries.emplace_back(binary(size, byte(i & 0xFF)));
		else
			boundaries.emplace_back(string(size, char('a' + i % 26)));
	}
	expected.insert(expected.end(), boundaries.begin(), boundaries.end());
	dc1->send(std::move(boundaries));

	// A batch larger than the send buffer is partly buffered, and what follows arrives after it
	dc1->onBufferedAmountLow([&lowCount]() { ++lowCount; });

	vector<message_variant> large;
	for (int i = 0; i < 256; ++i) {
		if (i % 4 == 0)
			large.emplace_back(binary(65536, byte(i & 0xFF)));
		else
			large.emplace_back("small " + to_string(i));
	}
	expected.insert(expected.end(), large.begin(), large.end());
	if (dc1->send(std::move(large)))
		return TestResult(false, "Batch larger than the send buffer was not buffered");

	expected.emplace_back(string("behind"));
	dc1->send(string("behind"));

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

	// The buffered amount is updated by the sending thread, possibly after the last message arrived
	attempts = 10;
	while ((dc1->bufferedAmount() > 0 || lowCount == 0) && attempts--)
		this_thread::sleep_for(100ms);

	if (dc1->bufferedAmount() > 0 || lowCount == 0)
		return TestResult(false, "Buffered amount did not go back to zero");

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}
