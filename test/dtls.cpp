/**
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace rtc;
using namespace std;
using namespace chrono_literals;

namespace {

struct ConnectionState {
	mutex lock;
	condition_variable changed;
	PeerConnection::IceState ice = PeerConnection::IceState::New;
	PeerConnection::GatheringState gathering = PeerConnection::GatheringState::New;
	bool channelOpen = false;
	bool closed = false;
	bool connected = false;
	bool failed = false;
};

template <typename Predicate>
bool waitFor(const shared_ptr<ConnectionState> &state, chrono::milliseconds timeout,
             Predicate predicate) {
	unique_lock lock(state->lock);
	return state->changed.wait_for(lock, timeout, [&] { return predicate(*state); });
}

void observe(PeerConnection &pc, const shared_ptr<ConnectionState> &state) {
	pc.onStateChange([state](PeerConnection::State value) {
		{
			lock_guard lock(state->lock);
			state->closed = state->closed || value == PeerConnection::State::Closed;
			state->connected = state->connected || value == PeerConnection::State::Connected;
			state->failed = state->failed || value == PeerConnection::State::Failed;
		}
		state->changed.notify_all();
	});
	pc.onIceStateChange([state](PeerConnection::IceState value) {
		{
			lock_guard lock(state->lock);
			state->ice = value;
		}
		state->changed.notify_all();
	});
	pc.onGatheringStateChange([state](PeerConnection::GatheringState value) {
		{
			lock_guard lock(state->lock);
			state->gathering = value;
		}
		state->changed.notify_all();
	});
}

Description corruptFingerprint(Description description) {
	auto fingerprint = description.fingerprint();
	if (!fingerprint || fingerprint->value.empty())
		throw runtime_error("Answer has no fingerprint");

	fingerprint->value.front() = fingerprint->value.front() == '0' ? '1' : '0';
	description.setFingerprint(*fingerprint);
	return description;
}

void runDelayedAnswer(bool invalidFingerprint) {
	Configuration config;
	config.disableAutoNegotiation = true;
#if RTC_ENABLE_MEDIA
	config.forceMediaTransport = true;
#endif

	PeerConnection offerer(config);
	PeerConnection answerer(config);
	auto offererState = make_shared<ConnectionState>();
	auto answererState = make_shared<ConnectionState>();
	observe(offerer, offererState);
	observe(answerer, answererState);

	auto channel = offerer.createDataChannel("dtls-startup-order");
	channel->onOpen([offererState] {
		{
			lock_guard lock(offererState->lock);
			offererState->channelOpen = true;
		}
		offererState->changed.notify_all();
	});
	offerer.setLocalDescription(Description::Type::Offer);
	if (!waitFor(offererState, 5s, [](const ConnectionState &state) {
		    return state.gathering == PeerConnection::GatheringState::Complete;
	    }))
		throw runtime_error("Offer gathering timed out");

	auto offer = offerer.localDescription();
	if (!offer || offer->candidates().empty())
		throw runtime_error("Full offer has no inline candidates");

	answerer.setRemoteDescription(*offer);
	answerer.setLocalDescription(Description::Type::Answer);
	if (!waitFor(answererState, 5s, [](const ConnectionState &state) {
		    return state.gathering == PeerConnection::GatheringState::Complete;
	    }))
		throw runtime_error("Answer gathering timed out");

	auto answer = answerer.localDescription();
	if (!answer || answer->candidates().empty())
		throw runtime_error("Full answer has no inline candidates");

	if (!waitFor(answererState, 5s, [](const ConnectionState &state) {
		    return state.ice == PeerConnection::IceState::Checking ||
		           state.ice == PeerConnection::IceState::Connected ||
		           state.ice == PeerConnection::IceState::Completed;
	    }))
		throw runtime_error("Answerer did not start ICE checks before the delayed answer");

	this_thread::sleep_for(50ms);
	offerer.setRemoteDescription(invalidFingerprint ? corruptFingerprint(*answer) : *answer);

	if (!waitFor(offererState, 10s, [invalidFingerprint](const ConnectionState &state) {
		    return state.failed || state.closed ||
		           (state.connected && (invalidFingerprint || state.channelOpen));
	    }))
		throw runtime_error("Offerer did not finish DTLS startup");

	{
		lock_guard lock(offererState->lock);
		if (invalidFingerprint &&
		    (offererState->connected || (!offererState->failed && !offererState->closed)))
			throw runtime_error("Invalid fingerprint did not fail authentication");
		if (!invalidFingerprint && !offererState->connected)
			throw runtime_error("Valid delayed answer did not connect");
	}

	if (!invalidFingerprint) {
		if (!waitFor(answererState, 10s, [](const ConnectionState &state) {
			    return state.connected || state.failed || state.closed;
		    }))
			throw runtime_error("Answerer did not finish DTLS startup");

		lock_guard lock(answererState->lock);
		if (!answererState->connected)
			throw runtime_error("Answerer did not connect");
	} else {
		this_thread::sleep_for(100ms);
	}

	offerer.close();
	answerer.close();
}

void runIterations(int iterations, bool invalidFingerprint) {
	for (int i = 0; i < iterations; ++i) {
		try {
			runDelayedAnswer(invalidFingerprint);
			if (Cleanup().wait_for(10s) == future_status::timeout)
				throw runtime_error("Cleanup timed out");
		} catch (const exception &e) {
			const string message = string(invalidFingerprint ? "Invalid" : "Valid") +
			                       " fingerprint iteration " + to_string(i + 1) + ": " + e.what();
			cerr << message << endl;
			throw runtime_error(message);
		}
	}
}

} // namespace

TestResult test_dtls_startup_after_remote_description() {
	InitLogger(LogLevel::Warning);

	runIterations(100, false);
	runIterations(10, true);

	return TestResult(true);
}
