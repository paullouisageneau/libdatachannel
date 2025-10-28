/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <chrono>
#include <iostream>
#include <thread>

#include "test.hpp"
#include <rtc/rtc.hpp>

using namespace std;
using namespace chrono_literals;

using chrono::duration_cast;
using chrono::milliseconds;
using chrono::seconds;
using chrono::steady_clock;

TestResult test_connectivity();
TestResult test_connectivity_fail_on_wrong_fingerprint();
TestResult test_pem();
TestResult test_negotiated();
TestResult test_reliability();
TestResult test_turn_connectivity();
TestResult test_track();
TestResult test_capi_connectivity();
TestResult test_capi_track();
TestResult test_websocket();
TestResult test_websocketserver();
TestResult test_capi_websocketserver();
size_t benchmark(chrono::milliseconds duration);

void test_benchmark() {
	size_t goodput = benchmark(10s);

	if (goodput == 0)
		throw runtime_error("No data received");

	const size_t threshold = 1000; // 1 MB/s;
	if (goodput < threshold)
		throw runtime_error("Goodput is too low");
}

TestResult test_cleanup() {
	try {
		// Every created object must have been destroyed, otherwise the wait will block
		if (rtc::Cleanup().wait_for(10s) == future_status::timeout)
			return TestResult(false, "timeout");
		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

TestResult test_capi_cleanup() {
	try {
		rtcCleanup();
		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

static const vector<Test> tests = {
    // C++ API tests
    Test("WebRTC connectivity", test_connectivity),
    Test("WebRTC broken fingerprint", test_connectivity_fail_on_wrong_fingerprint),
    Test("pem", test_pem),
    // TODO: Temporarily disabled as the Open Relay TURN server is unreliable
    // new Test("WebRTC TURN connectivity", test_turn_connectivity),
    Test("WebRTC negotiated DataChannel", test_negotiated),
    Test("WebRTC reliability mode", test_reliability),
#if RTC_ENABLE_MEDIA
    Test("WebRTC track", test_track),
#endif
#if RTC_ENABLE_WEBSOCKET
    // TODO: Temporarily disabled as the echo service is unreliable
    // new Test("WebSocket", test_websocket),
    Test("WebSocketServer", test_websocketserver),
#endif
    Test("Cleanup", test_cleanup),
    // C API tests
    Test("WebRTC C API connectivity", test_capi_connectivity),
#if RTC_ENABLE_MEDIA
    Test("WebRTC C API track", test_capi_track),
#endif
#if RTC_ENABLE_WEBSOCKET
    Test("WebSocketServer C API", test_capi_websocketserver),
#endif
    Test("C API cleanup", test_capi_cleanup),
};

int main(int argc, char **argv) {
	int success_tests = 0;
	int failed_tests = 0;
	steady_clock::time_point startTime, endTime;

	startTime = steady_clock::now();

	for (auto test : tests) {
		auto res = test.run();
		if (res.success) {
			success_tests++;
		} else {
			failed_tests++;
		}
	}

	endTime = steady_clock::now();

	auto durationMs = duration_cast<milliseconds>(endTime - startTime);
	auto durationS = duration_cast<seconds>(endTime - startTime);
	cout << "Finished " << success_tests + failed_tests << " tests in " << durationS.count()
	     << "s (" << durationMs.count() << " ms). Succeeded: " << success_tests
	     << ". Failed: " << failed_tests << "." << endl;
	/*
	    // Benchmark
	    try {
	        cout << endl << "*** Running WebRTC benchmark..." << endl;
	        test_benchmark();
	        cout << "*** Finished WebRTC benchmark" << endl;
	    } catch (const exception &e) {
	        cerr << "WebRTC benchmark failed: " << e.what() << endl;
	        std::this_thread::sleep_for(2s);
	        return -1;
	    }
	*/
	return 0;
}
