/**
 * Copyright (c) 2026 Nemirtingas
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

using namespace rtc;
using namespace std;
using namespace chrono_literals;

// Regression test for https://github.com/paullouisageneau/libdatachannel/issues/1616
// Certificates must still be generated when every thread of the thread pool is waiting for one,
// otherwise a small thread pool deadlocks.
TestResult test_certificate_small_thread_pool() {
	InitLogger(LogLevel::Debug);

	// Make sure the thread pool size is applied on next initialization
	if (Cleanup().wait_for(10s) == future_status::timeout)
		return TestResult(false, "Initial cleanup timeout");

	// The thread pool will have the minimum number of threads, i.e. 2
	SetThreadPoolSize(1);

	struct State {
		mutex mutex;
		condition_variable cv;
		int arrived = 0;
		int done = 0;
		vector<shared_ptr<PeerConnection>> peerConnections;
	};
	auto state = make_shared<State>();

	Configuration config;
	config.disableAutoNegotiation = true;

	// Each callback is called on a thread of the thread pool
	auto callback = [state, config](Description) {
		{
			unique_lock lock(state->mutex);
			++state->arrived;
			state->cv.notify_all();
			// Wait for the other callback so all the threads in the pool are busy
			state->cv.wait_for(lock, 5s, [&]() { return state->arrived >= 2; });
		}

		// The certificate generation for the new PeerConnection is not started yet as all the
		// threads in the pool are busy, and setLocalDescription() waits for the certificate
		auto pc = make_shared<PeerConnection>(config);
		auto dc = pc->createDataChannel("test");
		pc->setLocalDescription();

		unique_lock lock(state->mutex);
		state->peerConnections.push_back(std::move(pc));
		++state->done;
		state->cv.notify_all();
	};

	auto pc1 = make_shared<PeerConnection>(config);
	auto pc2 = make_shared<PeerConnection>(config);
	pc1->onLocalDescription(callback);
	pc2->onLocalDescription(callback);

	auto dc1 = pc1->createDataChannel("test");
	auto dc2 = pc2->createDataChannel("test");
	pc1->setLocalDescription();
	pc2->setLocalDescription();

	{
		unique_lock lock(state->mutex);
		if (!state->cv.wait_for(lock, 10s, [&]() { return state->done >= 2; })) {
			// Threads in the pool are deadlocked, intentionally leak the objects as destroying
			// them would block
			(void)new auto(
			    std::make_tuple(std::move(pc1), std::move(pc2), std::move(dc1), std::move(dc2)));
			return TestResult(false, "Timeout, certificate generation is deadlocked");
		}
	}

	pc1->close();
	pc2->close();
	{
		unique_lock lock(state->mutex);
		for (auto &pc : state->peerConnections)
			pc->close();

		state->peerConnections.clear();
	}
	pc1.reset();
	pc2.reset();

	if (Cleanup().wait_for(10s) == future_status::timeout)
		return TestResult(false, "Cleanup timeout");

	// Restore the thread pool size set in main()
	SetThreadPoolSize(4);

	cout << "Success" << endl;
	return TestResult(true);
}
