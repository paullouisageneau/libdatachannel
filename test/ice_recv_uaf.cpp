/**
 * Copyright (c) 2026 libdatachannel contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Regression stress test for the IceTransport teardown race against an
// in-flight libnice RecvCallback (originally observed as a heap-use-after-free
// in IceTransport::RecvCallback while ~IceTransport ran on the RTC worker).
//
// On a stock build the race window is narrow and this test only catches
// regressions probabilistically — it should be run under ASan or TSan in CI.
//
// Configure with -DRTC_TEST_RECV_DELAY=ON to inject a 50 ms sleep into
// IceTransport::RecvCallback. With the hook on, this test deterministically
// triggers the UAF on a broken tree within the first few iterations under a
// sanitizer. The hook is never compiled into production builds.

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std::chrono_literals;

namespace {

bool run_one_cycle() {
	Configuration cfg;
	cfg.iceServers.clear(); // host-only candidates; all-local

	auto pc1 = std::make_shared<PeerConnection>(cfg);
	auto pc2 = std::make_shared<PeerConnection>(cfg);

	pc1->onLocalDescription([&pc2](Description sdp) {
		pc2->setRemoteDescription(std::string(sdp));
	});
	pc1->onLocalCandidate([&pc2](Candidate c) {
		pc2->addRemoteCandidate(std::string(c));
	});
	pc2->onLocalDescription([&pc1](Description sdp) {
		pc1->setRemoteDescription(std::string(sdp));
	});
	pc2->onLocalCandidate([&pc1](Candidate c) {
		pc1->addRemoteCandidate(std::string(c));
	});

	std::shared_ptr<DataChannel> dc2;
	pc2->onDataChannel([&dc2](std::shared_ptr<DataChannel> dc) {
		std::atomic_store(&dc2, dc);
		dc->onMessage([wdc = std::weak_ptr<DataChannel>(dc)](auto) {
			if (auto d = wdc.lock())
				d->send("pong");
		});
	});

	auto dc1 = pc1->createDataChannel("uafrepro");
	std::atomic<bool> opened{false};
	dc1->onOpen([&opened, wdc = std::weak_ptr<DataChannel>(dc1)]() {
		opened = true;
		if (auto d = wdc.lock()) {
			// Burst messages so RecvCallback is actively dispatching
			// when we tear down below.
			for (int i = 0; i < 200; ++i)
				d->send("ping");
		}
	});
	dc1->onMessage([wdc = std::weak_ptr<DataChannel>(dc1)](auto) {
		if (auto d = wdc.lock())
			d->send("ping");
	});

	for (int i = 0; i < 300 && !opened; ++i)
		std::this_thread::sleep_for(10ms);

	if (!opened)
		return false;

	// Let data flow briefly so callbacks are pending in the libnice main
	// loop, then drop both peers — this is the moment the original UAF
	// fired.
	std::this_thread::sleep_for(50ms);
	pc1.reset();
	pc2.reset();
	return true;
}

} // namespace

TestResult test_ice_recv_uaf() {
	// 50 cycles is enough to occasionally hit the race under ASan/TSan
	// without blowing up CI wall time. A real regression keeps failing
	// across runs.
	constexpr int kIterations = 50;
	int opened = 0;
	for (int i = 0; i < kIterations; ++i) {
		if (run_one_cycle())
			++opened;
	}
	if (opened < kIterations / 2)
		return TestResult(false, "too few iterations actually connected; check local network");
	return TestResult(true);
}
