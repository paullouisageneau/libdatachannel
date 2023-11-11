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

#include <rtc/rtc.hpp>

using namespace std;
using namespace chrono_literals;

void test_connectivity(bool signal_wrong_fingerprint);
void test_negotiated();
void test_reliability();
void test_turn_connectivity();
void test_track();
void test_capi_connectivity();
void test_capi_track();
void test_websocket();
void test_websocketserver();
void test_capi_websocketserver();
size_t benchmark(chrono::milliseconds duration);

void test_benchmark() {
	size_t goodput = benchmark(10s);

	if (goodput == 0)
		throw runtime_error("No data received");

	const size_t threshold = 1000; // 1 MB/s;
	if (goodput < threshold)
		throw runtime_error("Goodput is too low");
}

int main(int argc, char **argv) {
	// C++ API tests
	try {
		cout << endl << "*** Running WebRTC connectivity test..." << endl;
		test_connectivity(false);
		cout << "*** Finished WebRTC connectivity test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC connectivity test failed: " << e.what() << endl;
		return -1;
	}
	try {
		cout << endl << "*** Running WebRTC broken fingerprint test..." << endl;
		test_connectivity(true);
		cerr << "WebRTC connectivity test failed to detect broken fingerprint" << endl;
		return -1;
	} catch (const exception &) {
	}

// TODO: Temporarily disabled as the Open Relay TURN server is unreliable
/*
	try {
		cout << endl << "*** Running WebRTC TURN connectivity test..." << endl;
		test_turn_connectivity();
		cout << "*** Finished WebRTC TURN connectivity test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC TURN connectivity test failed: " << e.what() << endl;
		return -1;
	}
*/
	try {
		cout << endl << "*** Running WebRTC negotiated DataChannel test..." << endl;
		test_negotiated();
		cout << "*** Finished WebRTC negotiated DataChannel test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC negotiated DataChannel test failed: " << e.what() << endl;
		return -1;
	}
	try {
		cout << endl << "*** Running WebRTC reliability mode test..." << endl;
		test_reliability();
		cout << "*** Finished WebRTC reliaility mode test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC reliability test failed: " << e.what() << endl;
		return -1;
	}
#if RTC_ENABLE_MEDIA
	try {
		cout << endl << "*** Running WebRTC Track test..." << endl;
		test_track();
		cout << "*** Finished WebRTC Track test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC Track test failed: " << e.what() << endl;
		return -1;
	}
#endif
#if RTC_ENABLE_WEBSOCKET
// TODO: Temporarily disabled as the echo service is unreliable
/*
	try {
		cout << endl << "*** Running WebSocket test..." << endl;
		test_websocket();
		cout << "*** Finished WebSocket test" << endl;
	} catch (const exception &e) {
		cerr << "WebSocket test failed: " << e.what() << endl;
		return -1;
	}
*/
	try {
		cout << endl << "*** Running WebSocketServer test..." << endl;
		test_websocketserver();
		cout << "*** Finished WebSocketServer test" << endl;
	} catch (const exception &e) {
		cerr << "WebSocketServer test failed: " << e.what() << endl;
		return -1;
	}
#endif
	try {
		// Every created object must have been destroyed, otherwise the wait will block
		cout << endl << "*** Running cleanup..." << endl;
		if(rtc::Cleanup().wait_for(10s) == future_status::timeout)
			throw std::runtime_error("Timeout");
		cout << "*** Finished cleanup..." << endl;
	} catch (const exception &e) {
		cerr << "Cleanup failed: " << e.what() << endl;
		return -1;
	}

	// C API tests
	try {
		cout << endl << "*** Running WebRTC C API connectivity test..." << endl;
		test_capi_connectivity();
		cout << "*** Finished WebRTC C API connectivity test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC C API connectivity test failed: " << e.what() << endl;
		return -1;
	}
#if RTC_ENABLE_MEDIA
	try {
		cout << endl << "*** Running WebRTC C API track test..." << endl;
		test_capi_track();
		cout << "*** Finished WebRTC C API track test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC C API track test failed: " << e.what() << endl;
		return -1;
	}
#endif
#if RTC_ENABLE_WEBSOCKET
	try {
		cout << endl << "*** Running WebSocketServer C API test..." << endl;
		test_capi_websocketserver();
		cout << "*** Finished WebSocketServer C API test" << endl;
	} catch (const exception &e) {
		cerr << "WebSocketServer C API test failed: " << e.what() << endl;
		return -1;
	}
#endif
	try {
		cout << endl << "*** Running C API cleanup..." << endl;
		rtcCleanup();
		cout << "*** Finished C API cleanup..." << endl;
	} catch (const exception &e) {
		cerr << "C API cleanup failed: " << e.what() << endl;
		return -1;
	}
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
