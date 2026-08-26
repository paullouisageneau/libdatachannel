/*
 * TURN/DTLS ABBA deadlock reproduction for libdatachannel v0.24.5
 *
 * Reproduces the symptom analysed in the Notion report
 * "libdatachannel TURN/DTLS deadlock analysis & fix (2026-08-24)":
 *
 *   In a RelayOnly (forced-TURN) topology, the DTLS *server (passive)* peer can
 *   wedge during the DTLS handshake. If a ClientHello is pre-queued in the
 *   incoming queue *before* DtlsTransport::start() runs, three locks form an
 *   ABBA cycle:
 *
 *     - juice poll thread: holds libjuice registry->mutex (callback context),
 *       waits on mSslMutex inside DtlsTransport::start()/handleTimeout().
 *     - ThreadPool doRecv thread: holds mSslMutex, sends the ServerHello flight
 *       and blocks in juice_send() waiting on conn_lock (== registry->mutex),
 *       but only on the *relay* send path (direct/srflx sends are lock-free).
 *
 *   => the two threads wait on each other forever. ICE reports the pair as
 *      selected (relay), but DTLS never completes and the connection dies at
 *      the ~30s consent-expiry timeout.
 *
 * This program forces BOTH peers onto the TURN relay path (TransportPolicy::
 * Relay) through a single fixed TURN instance, and runs many short-lived
 * connection attempts. Because both peers live in the same process the relay
 * RTT is tiny, so the remote ClientHello routinely lands in the same
 * millisecond as the local start() call -- exactly the race the report found.
 * A watchdog flags any attempt whose DTLS handshake does not finish in time.
 *
 * Expected behaviour:
 *   - Unpatched library (inline handleTimeout() at the end of
 *     DtlsTransport::start()): some attempts stall -- ICE connects (relay) but
 *     the DataChannel never opens.
 *   - Patched library (start() defers the initial timeout handling to the
 *     ThreadPool): every attempt completes the handshake in a few ms.
 *
 * Usage: turn-dtls-deadlock [iterations]
 *
 * TURN credentials are read from the RTC_TURN environment variable, e.g.
 *   set RTC_TURN=turn:<username>:<credential>@turn.server.ip.address:3478
 * If unset, the placeholder below is used (edit it before running).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

// Single fixed TURN instance, matching the field repro topology. Both peers are
// pinned to the relay path so every send takes the conn_lock code path.
static string turnServer() {
	if (const char *env = std::getenv("RTC_TURN"); env && *env)
		return string(env);
	// EDIT ME: fill in your TURN credential before running.
	return "turn:TurnUserName:TurnCredential@turn.server.ip.address:3478";
}

int main(int argc, char **argv) {
	InitLogger(LogLevel::Debug);

	const int iterations = (argc > 1) ? std::atoi(argv[1]) : 50;
	const auto handshakeBudget = 20s; // field consent-expiry was ~30s
	const string turn = turnServer();

	cout << "TURN relay: " << turn << endl;
	cout << "Forcing RelayOnly on both peers; " << iterations << " attempt(s)." << endl;

	int stalledCount = 0;
	for (int i = 1; i <= iterations; ++i) {
		cout << "\n===== attempt " << i << "/" << iterations << " =====" << endl;

		// pc1 = offerer. It advertises a=setup:actpass and ends up as the DTLS
		// server (passive) role -- the side vulnerable to the ABBA deadlock.
		Configuration config1;
		config1.iceServers.emplace_back(turn);
		config1.iceTransportPolicy = TransportPolicy::Relay; // force relay
		auto pc1 = make_shared<PeerConnection>(config1);

		// pc2 = answerer -> DTLS client (active); sends the ClientHello early.
		Configuration config2;
		config2.iceServers.emplace_back(turn);
		config2.iceTransportPolicy = TransportPolicy::Relay; // force relay
		auto pc2 = make_shared<PeerConnection>(config2);

		// In-process signaling: forward SDP and candidates between the peers.
		pc1->onLocalDescription([wpc2 = make_weak_ptr(pc2)](Description sdp) {
			if (auto pc2 = wpc2.lock())
				pc2->setRemoteDescription(string(sdp));
		});
		pc1->onLocalCandidate([wpc2 = make_weak_ptr(pc2)](Candidate cand) {
			if (auto pc2 = wpc2.lock())
				pc2->addRemoteCandidate(string(cand));
		});
		pc2->onLocalDescription([wpc1 = make_weak_ptr(pc1)](Description sdp) {
			if (auto pc1 = wpc1.lock())
				pc1->setRemoteDescription(string(sdp));
		});
		pc2->onLocalCandidate([wpc1 = make_weak_ptr(pc1)](Candidate cand) {
			if (auto pc1 = wpc1.lock())
				pc1->addRemoteCandidate(string(cand));
		});

		pc1->onStateChange([](PeerConnection::State s) { cout << "  [pc1] state: " << s << endl; });
		pc2->onStateChange([](PeerConnection::State s) { cout << "  [pc2] state: " << s << endl; });
		pc1->onIceStateChange(
		    [](PeerConnection::IceState s) { cout << "  [pc1] ice:   " << s << endl; });
		pc2->onIceStateChange(
		    [](PeerConnection::IceState s) { cout << "  [pc2] ice:   " << s << endl; });

		std::atomic<bool> opened = false; // set when the DTLS handshake completes

		shared_ptr<DataChannel> dc2;
		pc2->onDataChannel([&opened, &dc2](shared_ptr<DataChannel> dc) {
			dc->onOpen([&opened]() { opened = true; });
			std::atomic_store(&dc2, dc);
		});

		auto dc1 = pc1->createDataChannel("repro");
		dc1->onOpen([&opened]() { opened = true; });

		// Watchdog: time from start to a completed DTLS handshake.
		const auto begin = clock_type::now();
		bool stalled = true;
		while (clock_type::now() - begin < handshakeBudget) {
			if (opened.load()) {
				stalled = false;
				break;
			}
			this_thread::sleep_for(50ms);
		}

		if (stalled) {
			++stalledCount;
			const auto ice1 = pc1->iceState();
			const bool iceUp = (ice1 == PeerConnection::IceState::Connected ||
			                    ice1 == PeerConnection::IceState::Completed);
			cerr << "  !! DTLS handshake did NOT complete within "
			     << chrono::duration_cast<chrono::seconds>(handshakeBudget).count() << "s" << endl;
			cerr << "  !! pc1 ice=" << ice1 << " (relay selected=" << (iceUp ? "yes" : "no")
			     << "), signaling=" << pc1->signalingState() << endl;
			if (iceUp)
				cerr << "  !! Symptom matches the report: ICE relay pair selected but DTLS "
				        "never finishes -> DTLS server (passive) side is wedged in the ABBA "
				        "deadlock."
				     << endl;
		} else {
			cout << "  OK: DTLS handshake completed in "
			     << chrono::duration_cast<chrono::milliseconds>(clock_type::now() - begin).count()
			     << " ms" << endl;
		}

		if (!stalled) {
			// Healthy attempt: normal teardown is safe.
			pc1->close();
			pc2->close();
		} else {
			// A wedged attempt can NEVER be torn down: close() and
			// ~PeerConnection() must acquire the very mutexes the deadlocked
			// threads hold forever (dump #1: main blocked in _Mtx_lock inside
			// PeerConnection::close()). Nor can the loop continue: the wedged
			// "juice poll" thread keeps holding libjuice's global conn registry
			// mutex, shared by every agent in the process, so creating the next
			// attempt's PeerConnection would also hang forever on that mutex
			// (dump #2). Report and terminate immediately with std::_Exit; it
			// skips all destructors and atexit handlers, so the wedged objects
			// are implicitly leaked and nothing ever blocks on the way out.
			cout << "\n===== summary: " << stalledCount << "/" << i
			     << " attempt(s) stalled at the DTLS handshake =====" << endl;
			cout << "Reproduced the TURN/DTLS deadlock symptom." << endl;
			cout.flush();
			cerr.flush();
			std::_Exit(1);
		}
		this_thread::sleep_for(500ms);
	}

	// A stalled attempt already terminated the process with std::_Exit(1)
	// inside the loop, so reaching this point means every attempt succeeded.
	assert(stalledCount == 0);
	cout << "\n===== summary: 0/" << iterations
	     << " attempt(s) stalled at the DTLS handshake =====" << endl;
	cout << "No stall observed (expected on the patched library)." << endl;
	return 0;
}