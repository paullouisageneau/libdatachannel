/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

TestResult test_websocketserver() {
	InitLogger(LogLevel::Debug);

	WebSocketServer::Configuration serverConfig;
	serverConfig.port = 48080;
	serverConfig.enableTls = true;
	// serverConfig.certificatePemFile = ...
	// serverConfig.keyPemFile = ...
	serverConfig.bindAddress = "127.0.0.1"; // to test IPv4 fallback
	serverConfig.maxMessageSize = 1000;     // to test max message size
	WebSocketServer server(std::move(serverConfig));

	WebSocket::Headers requestHeaders = {
	    {"Authorization", "Bearer 9c96615b"},
	    {"User-Agent", "libdatachannel/0.24"},
	    {"X-Badly-Formatted", "Hello\r\nWorld"},
	};
	WebSocket::Headers expectedRequestHeaders = {
	    {"Authorization", "Bearer 9c96615b"},
	    {"User-Agent", "libdatachannel/0.24"},
	    {"X-Badly-Formatted", "Hello World"},
	};
	bool allRequestHeadersReceived = false;

	shared_ptr<WebSocket> client;
	server.onClient([&client, &expectedRequestHeaders,
	                 &allRequestHeadersReceived](shared_ptr<WebSocket> incoming) {
		cout << "WebSocketServer: Client connection received" << endl;
		client = incoming;

		if (auto addr = client->remoteAddress())
			cout << "WebSocketServer: Client remote address is " << *addr << endl;

		client->onOpen([wclient = make_weak_ptr(client), &expectedRequestHeaders,
		                &allRequestHeadersReceived]() {
			cout << "WebSocketServer: Client connection open" << endl;
			if (auto client = wclient.lock()) {
				if (auto path = client->path())
					cout << "WebSocketServer: Requested path is " << *path << endl;

				bool ok = true;
				auto receivedRequestHeaders = client->requestHeaders();
				for (auto const &[name, value] : expectedRequestHeaders) {
					auto it = receivedRequestHeaders.find(name);
					if (it == receivedRequestHeaders.end()) {
						cout << "WebSocketServer: Request header " << name << " not received"
						     << endl;
						ok = false;
						continue;
					}
					if (it->first != name) {
						// While HTTP headers are not case-sensitive, they should still be
						// transmitted with their original casing.
						cout << "WebSocketServer: Request header " << it->first
						     << " does not match expected case: " << name << endl;
						ok = false;
						continue;
					}
					if (it->second != value) {
						cout << "WebSocketServer: Request header " << it->first
						     << " value mismatch: Expected \"" << value << "\", received \""
						     << it->second << "\"" << endl;
						ok = false;
						continue;
					}
				}

				allRequestHeadersReceived = ok;
				if (allRequestHeadersReceived) {
					cout << "WebSocketServer: Received " << expectedRequestHeaders.size()
					     << " request headers: ";
					for (auto it = expectedRequestHeaders.begin();
					     it != expectedRequestHeaders.end(); it++) {
						if (it != expectedRequestHeaders.begin())
							cout << ", ";
						cout << it->first;
					}
					cout << endl;
				}
			}
		});

		client->onClosed([]() { cout << "WebSocketServer: Client connection closed" << endl; });

		client->onMessage([wclient = make_weak_ptr(client)](variant<binary, string> message) {
			if (auto client = wclient.lock())
				client->send(std::move(message));
		});
	});

	WebSocket::Configuration config;
	config.disableTlsVerification = true;
	WebSocket ws(std::move(config));

	const string myMessage = "Hello world from client";

	ws.onOpen([&ws, &myMessage]() {
		cout << "WebSocket: Open" << endl;
		ws.send(binary(1001, byte(0))); // test max message size
		ws.send(myMessage);
	});

	ws.onClosed([]() { cout << "WebSocket: Closed" << endl; });

	std::atomic<bool> received = false;
	std::atomic<bool> maxSizeReceived = false;
	ws.onMessage([&received, &maxSizeReceived, &myMessage](variant<binary, string> message) {
		if (holds_alternative<string>(message)) {
			string str = std::move(get<string>(message));
			if ((received = (str == myMessage)))
				cout << "WebSocket: Received expected message" << endl;
			else
				cout << "WebSocket: Received UNEXPECTED message" << endl;
		} else {
			binary bin = std::move(get<binary>(message));
			if ((maxSizeReceived = (bin.size() == 1000)))
				cout << "WebSocket: Received large message truncated at max size" << endl;
			else
				cout << "WebSocket: Received large message NOT TRUNCATED" << endl;
		}
	});

	ws.open("wss://localhost:48080/", requestHeaders);

	int attempts = 15;
	while ((!ws.isOpen() || !received) && attempts--)
		this_thread::sleep_for(1s);

	if (!ws.isOpen())
		return TestResult(false, "WebSocket is not open");

	if (!received || !maxSizeReceived)
		return TestResult(false, "Expected messages not received");

	if (!allRequestHeadersReceived)
		return TestResult(false, "Some request headers not received");

	ws.close();
	this_thread::sleep_for(1s);

	server.stop();
	this_thread::sleep_for(1s);

	return TestResult(true);
}

// Regression test for https://github.com/paullouisageneau/libdatachannel/issues/1618
// A very short ping interval makes the TCP read timeout fire continuously, so the TLS transport
// keeps receiving zero-sized messages, which make the WebSocket send pings while other threads
// are sending messages and echoes are being received. With the Mbed TLS backend, this used to
// deadlock as pings were sent while the TLS transport mutex was held by the receive path.
TestResult test_websocketserver_tls_concurrent_send_recv() {
	InitLogger(LogLevel::Warning);

	const int threadsCount = 4;
	const int messagesPerThread = 500;
	const int expectedCount = threadsCount * messagesPerThread;

	WebSocketServer::Configuration serverConfig;
	serverConfig.port = 48082;
	serverConfig.enableTls = true;
	serverConfig.bindAddress = "127.0.0.1";
	// Objects are intentionally leaked if a deadlock is detected, as they can't be destroyed
	auto server = make_shared<WebSocketServer>(std::move(serverConfig));

	auto clients = make_shared<vector<shared_ptr<WebSocket>>>();
	server->onClient([clients](shared_ptr<WebSocket> incoming) {
		clients->push_back(incoming);
		incoming->onMessage([wclient = make_weak_ptr(incoming)](variant<binary, string> message) {
			if (auto client = wclient.lock()) {
				try {
					client->send(std::move(message));
				} catch (const exception &e) {
					cout << "WebSocketServer: Echo failed: " << e.what() << endl;
				}
			}
		});
	});

	WebSocket::Configuration config;
	config.disableTlsVerification = true;
	config.pingInterval = 1ms; // ping on every short idle period
	auto ws = make_shared<WebSocket>(std::move(config));

	auto receivedCount = make_shared<std::atomic<int>>(0);
	ws->onMessage([receivedCount](variant<binary, string>) { ++*receivedCount; });

	ws->open("wss://localhost:48082/");

	int attempts = 10;
	while (!ws->isOpen() && attempts--)
		this_thread::sleep_for(1s);

	if (!ws->isOpen())
		return TestResult(false, "WebSocket is not open");

	cout << "WebSocket: Open, sending messages from " << threadsCount << " threads" << endl;

	auto finishedThreads = make_shared<std::atomic<int>>(0);
	auto sendFailed = make_shared<std::atomic<bool>>(false);
	vector<thread> threads;
	for (int t = 0; t < threadsCount; ++t) {
		threads.emplace_back([ws, finishedThreads, sendFailed, t, messagesPerThread]() {
			try {
				for (int i = 0; i < messagesPerThread; ++i) {
					ws->send("Thread " + to_string(t) + " message " + to_string(i));
					// Leave idle periods so read timeouts happen while sending and receiving
					if (i % 10 == 0)
						this_thread::sleep_for(2ms);
				}
			} catch (const exception &e) {
				cout << "WebSocket: Send failed: " << e.what() << endl;
				*sendFailed = true;
			}
			++*finishedThreads;
		});
	}

	// Watchdog: fail instead of hanging forever if a deadlock occurs
	auto deadline = chrono::steady_clock::now() + 30s;
	while ((*finishedThreads < threadsCount || *receivedCount < expectedCount) &&
	       chrono::steady_clock::now() < deadline)
		this_thread::sleep_for(100ms);

	if (*finishedThreads < threadsCount) {
		// Threads are stuck, leak everything so the test can report the failure
		for (auto &th : threads)
			th.detach();
		new shared_ptr<WebSocket>(ws);
		new shared_ptr<WebSocketServer>(server);
		new shared_ptr<vector<shared_ptr<WebSocket>>>(clients);
		return TestResult(false, "Deadlock detected: send threads are blocked");
	}

	for (auto &th : threads)
		th.join();

	if (*sendFailed)
		return TestResult(false, "Send failed");

	if (*receivedCount < expectedCount)
		return TestResult(false, "Missing echoed messages: received " +
		                             to_string(receivedCount->load()) + " out of " +
		                             to_string(expectedCount));

	cout << "WebSocket: Received all " << expectedCount << " echoed messages" << endl;

	ws->close();
	this_thread::sleep_for(1s);

	server->stop();
	this_thread::sleep_for(1s);

	return TestResult(true);
}

#endif
