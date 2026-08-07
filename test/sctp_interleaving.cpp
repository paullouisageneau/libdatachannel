/**
 * Copyright (c) 2026 Mertushka
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

namespace {

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

bool waitUntil(function<bool()> condition, chrono::seconds timeout) {
	const auto deadline = chrono::steady_clock::now() + timeout;
	while (chrono::steady_clock::now() < deadline) {
		if (condition())
			return true;
		this_thread::sleep_for(100ms);
	}
	return condition();
}

binary makePattern(size_t size) {
	binary data(size);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = rtc::byte((i * 31 + 17) % 251);
	return data;
}

} // namespace

TestResult test_sctp_interleaving() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	config1.maxMessageSize = 2 * 1024 * 1024;
	PeerConnection pc1(config1);

	Configuration config2;
	config2.maxMessageSize = 2 * 1024 * 1024;
	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(string(sdp)); });
	pc1.onLocalCandidate([&pc2](Candidate candidate) { pc2.addRemoteCandidate(string(candidate)); });
	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });
	pc2.onLocalCandidate([&pc1](Candidate candidate) { pc1.addRemoteCandidate(string(candidate)); });

	mutex failureMutex;
	string failure;
	atomic<bool> failed = false;
	auto fail = [&](string reason) {
		lock_guard lock(failureMutex);
		if (!failed) {
			failure = std::move(reason);
			failed = true;
		}
	};

	const binary largeMessage = makePattern(384 * 1024);
	const int expectedLargeMessages = 2;
	const int expectedReliableMessages = 20;
	const int expectedUnreliableMessages = 4;
	atomic<int> largeReceived = 0;
	atomic<int> reliableReceived = 0;
	atomic<int> unreliableReceived = 0;

	shared_ptr<DataChannel> bulk2;
	shared_ptr<DataChannel> reliable2;
	shared_ptr<DataChannel> unreliable2;

	pc2.onDataChannel([&](shared_ptr<DataChannel> dc) {
		const auto label = dc->label();
		if (label == "interleave-bulk") {
			dc->onMessage([&, wdc = make_weak_ptr(dc)](const variant<binary, string> &message) {
				if (!wdc.lock())
					return;
				if (!holds_alternative<binary>(message)) {
					fail("Bulk channel received a non-binary message");
					return;
				}
				const auto &data = get<binary>(message);
				if (data != largeMessage) {
					fail("Bulk channel received corrupted binary data");
					return;
				}
				++largeReceived;
			});
			std::atomic_store(&bulk2, dc);
		} else if (label == "interleave-reliable") {
			dc->onMessage([&](const variant<binary, string> &message) {
				if (!holds_alternative<string>(message)) {
					fail("Reliable small channel received a non-string message");
					return;
				}
				const auto &text = get<string>(message);
				if (text.rfind("reliable-", 0) != 0) {
					fail("Reliable small channel received corrupted string data");
					return;
				}
				++reliableReceived;
			});
			std::atomic_store(&reliable2, dc);
		} else if (label == "interleave-unreliable") {
			dc->onMessage([&](const variant<binary, string> &message) {
				if (!holds_alternative<string>(message)) {
					fail("Unreliable small channel received a non-string message");
					return;
				}
				const auto &text = get<string>(message);
				if (text.rfind("unreliable-", 0) != 0) {
					fail("Unreliable small channel received corrupted string data");
					return;
				}
				++unreliableReceived;
			});
			std::atomic_store(&unreliable2, dc);
		} else {
			fail("Unexpected DataChannel label: " + label);
		}
	});

	auto bulk1 = pc1.createDataChannel("interleave-bulk");

	Reliability reliable;
	reliable.unordered = true;
	auto reliable1 = pc1.createDataChannel("interleave-reliable", {reliable});

	Reliability unreliable;
	unreliable.unordered = true;
	unreliable.maxRetransmits = 0;
	auto unreliable1 = pc1.createDataChannel("interleave-unreliable", {unreliable});

	if (!waitUntil(
	        [&] {
		        auto rb = std::atomic_load(&bulk2);
		        auto rr = std::atomic_load(&reliable2);
		        auto ru = std::atomic_load(&unreliable2);
		        return rb && rr && ru && bulk1->isOpen() && reliable1->isOpen() &&
		               unreliable1->isOpen() && rb->isOpen() && rr->isOpen() && ru->isOpen();
	        },
	        15s)) {
		pc1.close();
		pc2.close();
		return TestResult(false, "DataChannels are not open");
	}

	for (int round = 0; round < expectedLargeMessages; ++round) {
		bulk1->send(largeMessage);
		for (int i = 0; i < expectedReliableMessages / expectedLargeMessages; ++i)
			reliable1->send("reliable-" + to_string(round) + "-" + to_string(i));
	}
	for (int i = 0; i < expectedUnreliableMessages; ++i)
		unreliable1->send("unreliable-" + to_string(i));

	const bool delivered = waitUntil(
	    [&] {
		    return failed || (largeReceived == expectedLargeMessages &&
		                      reliableReceived == expectedReliableMessages &&
		                      unreliableReceived > 0);
	    },
	    15s);

	pc1.close();
	pc2.close();
	this_thread::sleep_for(1s);

	if (failed) {
		lock_guard lock(failureMutex);
		return TestResult(false, failure);
	}
	if (!delivered)
		return TestResult(false, "Timed out waiting for interleaved data-channel messages");
	if (unreliableReceived == 0)
		return TestResult(false, "Unreliable maxRetransmits=0 channel did not deliver any message");

	return TestResult(true);
}
