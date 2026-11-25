/**
 * Copyright (c) 2026 mertushka
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Compares a send() loop with a single send() of a vector for bursts of small messages.
// Both peers run in this process over loopback, so CPU time covers both ends.
// Rounds alternate between modes so that background load affects both equally.
//
// Usage: batch-benchmark [--sizes=64,256,1024] [--count=100000] [--batch=256] [--rounds=3]

#include "rtc/rtc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

using namespace rtc;
using namespace std;
using namespace chrono_literals;

using chrono::duration;
using chrono::steady_clock;

namespace {

// Process CPU time (user + system) in seconds
double processCpuSeconds() {
#ifdef _WIN32
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
		return 0.0;
	auto toSeconds = [](const FILETIME &ft) {
		ULARGE_INTEGER value;
		value.LowPart = ft.dwLowDateTime;
		value.HighPart = ft.dwHighDateTime;
		return double(value.QuadPart) * 1e-7;
	};
	return toSeconds(kernel) + toSeconds(user);
#else
	struct rusage usage = {};
	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return 0.0;
	auto toSeconds = [](const struct timeval &tv) {
		return double(tv.tv_sec) + double(tv.tv_usec) * 1e-6;
	};
	return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
#endif
}

struct Result {
	double messagesPerSecond;
	double cpuMicrosPerMessage;
};

Result run(bool batched, size_t messageSize, size_t count, size_t batchSize) {
	const size_t highWater = 1024 * 1024;
	const size_t lowWater = 256 * 1024;

	// Declared before the peers so it outlives any late callback
	mutex mtx;
	condition_variable cv;
	bool open = false;
	bool done = false;
	bool closed = false;
	size_t received = 0;
	steady_clock::time_point lastReceived;

	PeerConnection pc1;
	PeerConnection pc2;

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(std::move(sdp)); });
	pc1.onLocalCandidate([&pc2](Candidate candidate) {
		pc2.addRemoteCandidate(std::move(candidate));
	});
	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(std::move(sdp)); });
	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		pc1.addRemoteCandidate(std::move(candidate));
	});

	shared_ptr<DataChannel> dc2;
	pc2.onDataChannel([&](shared_ptr<DataChannel> dc) {
		dc->onClosed([&]() {
			lock_guard lock(mtx);
			closed = true;
			cv.notify_all();
		});
		dc->onMessage([&](message_variant) {
			lock_guard lock(mtx);
			if (++received == count) {
				lastReceived = steady_clock::now();
				done = true;
				cv.notify_all();
			}
		});
		lock_guard lock(mtx);
		dc2 = std::move(dc);
	});

	auto dc1 = pc1.createDataChannel("batch-benchmark");
	dc1->setBufferedAmountLowThreshold(lowWater);
	dc1->onOpen([&]() {
		lock_guard lock(mtx);
		open = true;
		cv.notify_all();
	});
	dc1->onBufferedAmountLow([&]() { cv.notify_all(); });

	{
		unique_lock lock(mtx);
		if (!cv.wait_for(lock, 10s, [&] { return open; }))
			throw runtime_error("DataChannel did not open");
	}

	const binary payload(messageSize, byte(0xFF));
	const double cpuStart = processCpuSeconds();
	const auto start = steady_clock::now();

	size_t sent = 0;
	while (sent < count) {
		if (dc1->bufferedAmount() >= highWater) {
			// Wait for onBufferedAmountLow, polling in case the callback raced the check
			unique_lock lock(mtx);
			cv.wait_for(lock, 1ms, [&] { return dc1->bufferedAmount() < highWater; });
			continue;
		}

		const size_t n = min(batchSize, count - sent);
		if (batched) {
			vector<message_variant> messages(n, payload);
			dc1->send(std::move(messages));
		} else {
			for (size_t i = 0; i < n; ++i)
				dc1->send(payload);
		}
		sent += n;
	}

	{
		unique_lock lock(mtx);
		if (!cv.wait_for(lock, 60s, [&] { return done; }))
			throw runtime_error("Only " + to_string(received) + " of " + to_string(count) +
			                    " messages received");
	}

	const double seconds = duration<double>(lastReceived - start).count();
	const double cpu = processCpuSeconds() - cpuStart;

	// Close gracefully so no data is in flight when the peers go away
	dc1->close();
	{
		unique_lock lock(mtx);
		cv.wait_for(lock, 10s, [&] { return closed; });
	}

	dc1->resetCallbacks();
	pc1.resetCallbacks();
	pc2.resetCallbacks();
	{
		lock_guard lock(mtx);
		if (dc2)
			dc2->resetCallbacks();
	}
	pc1.close();
	pc2.close();

	return Result{double(count) / seconds, cpu * 1e6 / double(count)};
}

double median(vector<double> values) {
	sort(values.begin(), values.end());
	return values[values.size() / 2];
}

bool parseOption(const char *arg, const char *name, string &value) {
	const size_t len = strlen(name);
	if (strncmp(arg, name, len) != 0 || arg[len] != '=')
		return false;
	value = arg + len + 1;
	return true;
}

} // namespace

int main(int argc, char **argv) {
	vector<size_t> sizes = {64, 256, 1024};
	size_t count = 100000;
	size_t batchSize = 256;
	int rounds = 3;

	try {
		for (int i = 1; i < argc; ++i) {
			string value;
			if (parseOption(argv[i], "--sizes", value)) {
				sizes.clear();
				stringstream ss(value);
				string item;
				while (getline(ss, item, ','))
					sizes.push_back(stoul(item));
			} else if (parseOption(argv[i], "--count", value)) {
				count = stoul(value);
			} else if (parseOption(argv[i], "--batch", value)) {
				batchSize = stoul(value);
			} else if (parseOption(argv[i], "--rounds", value)) {
				rounds = stoi(value);
			} else {
				cerr << "Usage: " << argv[0]
				     << " [--sizes=64,256,1024] [--count=100000] [--batch=256] [--rounds=3]"
				     << endl;
				return 1;
			}
		}
		if (sizes.empty() || count == 0 || batchSize == 0 || rounds <= 0)
			throw invalid_argument("Options must be positive");

		InitLogger(LogLevel::Warning);
		Preload();

		cout << count << " messages per run, batches of " << batchSize << ", " << rounds
		     << " alternating rounds, medians" << endl;
		cout << "size      send() msg/s         batch msg/s   speedup   send() CPU us/msg   "
		        "      batch CPU us/msg"
		     << endl;

		for (size_t size : sizes) {
			vector<double> sendRate, batchRate, sendCpu, batchCpu, ratio;
			for (int r = 0; r < rounds; ++r) {
				// Alternate which mode runs first so drift does not favor either
				Result s, b;
				if (r % 2 == 0) {
					s = run(false, size, count, batchSize);
					b = run(true, size, count, batchSize);
				} else {
					b = run(true, size, count, batchSize);
					s = run(false, size, count, batchSize);
				}
				sendRate.push_back(s.messagesPerSecond);
				batchRate.push_back(b.messagesPerSecond);
				sendCpu.push_back(s.cpuMicrosPerMessage);
				batchCpu.push_back(b.cpuMicrosPerMessage);
				ratio.push_back(b.messagesPerSecond / s.messagesPerSecond);
			}

			char line[160];
			snprintf(line, sizeof(line), "%-8zu  %12.0f   %17.0f   %6.2fx   %17.2f   %22.2f", size,
			         median(sendRate), median(batchRate), median(ratio), median(sendCpu),
			         median(batchCpu));
			cout << line << endl;
		}

		Cleanup();
		return 0;

	} catch (const std::exception &e) {
		cerr << "Batch benchmark failed: " << e.what() << endl;
		return -1;
	}
}
