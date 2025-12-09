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

	std::map<string, string, case_insensitive_less> requestHeaders = {
	    {"Authorization", "Bearer 9c96615b"},
	    {"User-Agent", "libdatachannel/0.24"},
	    {"X-Badly-Formatted", "Hello\r\nWorld"},
	};
	std::map<string, string, case_insensitive_less> expectedRequestHeaders = {
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

#endif
