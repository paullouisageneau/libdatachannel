/**
 * Copyright (c) 2026 Nemirtingas
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace rtc;
using namespace std;
using namespace chrono_literals;

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t InvalidSocket = INVALID_SOCKET;
void close_socket(socket_t sock) { closesocket(sock); }
#else
using socket_t = int;
const socket_t InvalidSocket = -1;
void close_socket(socket_t sock) { close(sock); }
#endif

// Invalid DTLS records which implementations must silently discard (RFC 6347 section 4.1.2.7)
const vector<string> Forgeries = {
    // Handshake record with a TLS 1.2 record version instead of DTLS 1.2
    string("\x16\x03\x03\x00\x00\x00\x00\x00\x00\x00\x02\x00\x0c"
           "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
           25),
    // Handshake record with garbage version and length
    string("\x16\x3b\x60\x85\xaa\xcf\xf4\x19\x3e\x63\x88\xad\xd2\xf7\x1c\x41", 16),
};

// UDP relay between two peers, injecting forged datagrams before DTLS datagrams
class Relay {
public:
	Relay(string ip) : mIp(std::move(ip)) {
		mSockA = bindSocket(mPortA);
		mSockB = bindSocket(mPortB);
		mThread = thread([this]() { run(); });
	}

	~Relay() {
		mRunning = false;
		mThread.join();
		close_socket(mSockA);
		close_socket(mSockB);
	}

	uint16_t portA() const { return mPortA; } // advertised to the answerer for the offerer
	uint16_t portB() const { return mPortB; } // advertised to the offerer for the answerer

	void setOfferer(uint16_t port) {
		mOfferer.sin_family = AF_INET;
		inet_pton(AF_INET, mIp.c_str(), &mOfferer.sin_addr);
		mOfferer.sin_port = htons(port);
	}

	int injectedToOfferer() const { return mInjectedToOfferer; }
	int injectedToAnswerer() const { return mInjectedToAnswerer; }

private:
	socket_t bindSocket(uint16_t &port) {
		socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == InvalidSocket)
			throw runtime_error("Failed to create UDP socket");

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		inet_pton(AF_INET, mIp.c_str(), &addr.sin_addr);
		addr.sin_port = 0;
		if (::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
			close_socket(sock);
			throw runtime_error("Failed to bind UDP socket");
		}

		socklen_t len = sizeof(addr);
		getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len);
		port = ntohs(addr.sin_port);
		return sock;
	}

	void forward(socket_t sock, const sockaddr_in &to, const char *data, int len,
	             atomic<int> &injected) {
		const auto first = static_cast<unsigned char>(data[0]);
		const bool dtls = first >= 20 && first <= 63 && first != 23; // excluding application data
		if (dtls && injected < 8) {
			const string &forged = Forgeries[size_t(injected) % Forgeries.size()];
			sendto(sock, forged.data(), int(forged.size()), 0,
			       reinterpret_cast<const sockaddr *>(&to), sizeof(to));
			++injected;
		}
		sendto(sock, data, len, 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
	}

	void run() {
		char buffer[65536];
		while (mRunning) {
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(mSockA, &fds);
			FD_SET(mSockB, &fds);
			timeval tv = {0, 100000};
			int nfds = int(std::max(mSockA, mSockB)) + 1;
			if (select(nfds, &fds, nullptr, nullptr, &tv) <= 0)
				continue;

			if (FD_ISSET(mSockA, &fds)) { // from the answerer
				sockaddr_in from = {};
				socklen_t fromLen = sizeof(from);
				int len = int(recvfrom(mSockA, buffer, sizeof(buffer), 0,
				                       reinterpret_cast<sockaddr *>(&from), &fromLen));
				if (len > 0) {
					mAnswerer = from;
					mAnswererKnown = true;
					forward(mSockB, mOfferer, buffer, len, mInjectedToOfferer);
				}
			}
			if (FD_ISSET(mSockB, &fds)) { // from the offerer
				int len = int(recvfrom(mSockB, buffer, sizeof(buffer), 0, nullptr, nullptr));
				if (len > 0 && mAnswererKnown)
					forward(mSockA, mAnswerer, buffer, len, mInjectedToAnswerer);
			}
		}
	}

	const string mIp;
	socket_t mSockA = InvalidSocket, mSockB = InvalidSocket;
	uint16_t mPortA = 0, mPortB = 0;
	sockaddr_in mOfferer = {}, mAnswerer = {};
	atomic<bool> mAnswererKnown = false;
	atomic<bool> mRunning = true;
	atomic<int> mInjectedToOfferer = 0, mInjectedToAnswerer = 0;
	thread mThread;
};

// Find the address and port of the first IPv4 host candidate
bool find_host_candidate(const string &sdp, string &ip, uint16_t &port) {
	istringstream iss(sdp);
	string line;
	while (getline(iss, line)) {
		if (line.rfind("a=candidate:", 0) != 0)
			continue;

		istringstream ls(line);
		string foundation, component, transport, priority, address, p, typ, type;
		ls >> foundation >> component >> transport >> priority >> address >> p >> typ >> type;
		if (type == "host" && address.find(':') == string::npos) {
			ip = address;
			port = uint16_t(stoi(p));
			return true;
		}
	}
	return false;
}

// Replace all candidates with a single one
string replace_candidates(const string &sdp, const string &ip, uint16_t port) {
	istringstream iss(sdp);
	ostringstream oss;
	string line;
	bool added = false;
	while (getline(iss, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.rfind("a=candidate:", 0) == 0) {
			if (!added) {
				oss << "a=candidate:1 1 UDP 2122317823 " << ip << " " << port << " typ host\r\n";
				added = true;
			}
			continue;
		}
		oss << line << "\r\n";
	}
	return oss.str();
}

template <typename Predicate> bool wait_until(Predicate predicate, chrono::milliseconds timeout) {
	auto end = chrono::steady_clock::now() + timeout;
	while (!predicate()) {
		if (chrono::steady_clock::now() > end)
			return false;

		this_thread::sleep_for(50ms);
	}
	return true;
}

} // namespace

// Forged datagrams injected during the DTLS handshake must not make it fail
TestResult test_dtls_handshake_with_forged_records() {
	InitLogger(LogLevel::Debug);

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return TestResult(false, "WSAStartup failed");
	struct WsaCleanup {
		~WsaCleanup() { WSACleanup(); }
	} wsaCleanup;
#endif

	try {
		Configuration config;
		config.bindAddress = "127.0.0.1"; // libjuice only
		config.disableAutoNegotiation = true;

		PeerConnection pc1(config);
		PeerConnection pc2(config);

		atomic<bool> gathered1 = false, gathered2 = false;
		pc1.onGatheringStateChange([&](PeerConnection::GatheringState state) {
			gathered1 = state == PeerConnection::GatheringState::Complete;
		});
		pc2.onGatheringStateChange([&](PeerConnection::GatheringState state) {
			gathered2 = state == PeerConnection::GatheringState::Complete;
		});

		atomic<bool> failed = false;
		auto onState = [&failed](const char *name) {
			return [&failed, name](PeerConnection::State state) {
				cout << "State " << name << ": " << state << endl;
				if (state == PeerConnection::State::Failed)
					failed = true;
			};
		};
		pc1.onStateChange(onState("1"));
		pc2.onStateChange(onState("2"));

		atomic<bool> received1 = false, received2 = false;
		auto dc1 = pc1.createDataChannel("test");
		dc1->onOpen([&dc1]() { dc1->send("Hello from 1"); });
		dc1->onMessage([&](message_variant) { received1 = true; });

		shared_ptr<DataChannel> dc2;
		pc2.onDataChannel([&](shared_ptr<DataChannel> dc) {
			dc->onMessage([&](message_variant) { received2 = true; });
			dc->send("Hello from 2");
			std::atomic_store(&dc2, dc);
		});

		pc1.setLocalDescription();
		if (!wait_until([&]() { return gathered1.load(); }, 10s))
			return TestResult(false, "Gathering timeout for peer 1");

		string offer = string(*pc1.localDescription());
		string ip;
		uint16_t port1 = 0;
		if (!find_host_candidate(offer, ip, port1))
			return TestResult(false, "No IPv4 host candidate for peer 1");

		Relay relay(ip);
		relay.setOfferer(port1);
		pc2.setRemoteDescription(
		    Description(replace_candidates(offer, ip, relay.portA()), "offer"));
		pc2.setLocalDescription();
		if (!wait_until([&]() { return gathered2.load(); }, 10s))
			return TestResult(false, "Gathering timeout for peer 2");

		string answer = string(*pc2.localDescription());
		pc1.setRemoteDescription(
		    Description(replace_candidates(answer, ip, relay.portB()), "answer"));

		if (!wait_until([&]() { return (received1 && received2) || failed; }, 20s))
			return TestResult(false, "DataChannel exchange timeout");

		cout << "Forged datagrams injected: " << relay.injectedToOfferer() << " to peer 1, "
		     << relay.injectedToAnswerer() << " to peer 2" << endl;

		if (failed)
			return TestResult(false, "PeerConnection failed");

		if (relay.injectedToOfferer() == 0 || relay.injectedToAnswerer() == 0)
			return TestResult(false, "No forged datagram injected");

		pc1.close();
		pc2.close();
		this_thread::sleep_for(1s);
		return TestResult(true);

	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}
