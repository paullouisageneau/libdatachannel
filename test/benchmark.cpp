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
using namespace chrono_literals;

using chrono::duration_cast;
using chrono::milliseconds;
using chrono::steady_clock;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

size_t benchmark(milliseconds duration) {
	rtc::InitLogger(LogLevel::Warning);
	rtc::Preload();

	Configuration config1;
	// config1.iceServers.emplace_back("stun:stun.l.google.com:19302");
	// config1.mtu = 1500;

	PeerConnection pc1(config1);

	Configuration config2;
	// config2.iceServers.emplace_back("stun:stun.l.google.com:19302");
	// config2.mtu = 1500;

	PeerConnection pc2(config2);

	pc1.onLocalDescription([&pc2](Description sdp) {
		cout << "Description 1: " << sdp << endl;
		pc2.setRemoteDescription(std::move(sdp));
	});

	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		cout << "Candidate 1: " << candidate << endl;
		pc2.addRemoteCandidate(std::move(candidate));
	});

	pc1.onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });
	pc1.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
	});

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(std::move(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		cout << "Candidate 2: " << candidate << endl;
		pc1.addRemoteCandidate(std::move(candidate));
	});

	pc2.onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });
	pc2.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	const size_t messageSize = 65535;
	binary messageData(messageSize);
	fill(messageData.begin(), messageData.end(), byte(0xFF));

	atomic<size_t> receivedSize = 0;

	steady_clock::time_point startTime, openTime, receivedTime, endTime;

	shared_ptr<DataChannel> dc2;
	pc2.onDataChannel([&dc2, &receivedSize, &receivedTime](shared_ptr<DataChannel> dc) {
		dc->onMessage([&receivedTime, &receivedSize](variant<binary, string> message) {
			if (holds_alternative<binary>(message)) {
				const auto &bin = get<binary>(message);
				if (receivedSize == 0)
					receivedTime = steady_clock::now();
				receivedSize += bin.size();
			}
		});

		dc->onClosed([]() { cout << "DataChannel closed." << endl; });

		std::atomic_store(&dc2, dc);
	});

	startTime = steady_clock::now();
	auto dc1 = pc1.createDataChannel("benchmark");

	dc1->onOpen([wdc1 = make_weak_ptr(dc1), &messageData, &openTime]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		openTime = steady_clock::now();

		cout << "DataChannel open, sending data..." << endl;
		try {
			while (dc1->bufferedAmount() == 0) {
				dc1->send(messageData);
			}
		} catch (const std::exception &e) {
			std::cout << "Send failed: " << e.what() << std::endl;
		}
	});

	// When sent data is buffered in the DataChannel,
	// wait for onBufferedAmountLow callback to continue
	dc1->onBufferedAmountLow([wdc1 = make_weak_ptr(dc1), &messageData]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		// Continue sending
		try {
			while (dc1->isOpen() && dc1->bufferedAmount() == 0) {
				dc1->send(messageData);
			}
		} catch (const std::exception &e) {
			std::cout << "Send failed: " << e.what() << std::endl;
		}
	});

	const int steps = 10;
	const auto stepDuration = duration / 10;
	for (int i = 0; i < steps; ++i) {
		this_thread::sleep_for(stepDuration);
		cout << "Received: " << receivedSize.load() / 1000 << " KB" << endl;
	}

	dc1->close();

	endTime = steady_clock::now();

	auto connectDuration = duration_cast<milliseconds>(dc1->isOpen() ? openTime - startTime
	                                                                 : steady_clock::duration(0));
	auto transferDuration = duration_cast<milliseconds>(endTime - receivedTime);

	cout << "Test duration: " << duration.count() << " ms" << endl;
	cout << "Connect duration: " << connectDuration.count() << " ms" << endl;

	size_t received = receivedSize.load();
	size_t goodput = transferDuration.count() > 0 ? received / transferDuration.count() : 0;
	cout << "Goodput: " << goodput * 0.001 << " MB/s"
	     << " (" << goodput * 0.001 * 8 << " Mbit/s)" << endl;

	pc1.close();
	pc2.close();

	rtc::Cleanup();
	return goodput;
}

#ifdef BENCHMARK_MAIN
int main(int argc, char **argv) {
	try {
		size_t goodput = benchmark(30s);
		if (goodput == 0)
			throw runtime_error("No data received");

		return 0;

	} catch (const std::exception &e) {
		cerr << "Benchmark failed: " << e.what() << endl;
		return -1;
	}
}
#endif
