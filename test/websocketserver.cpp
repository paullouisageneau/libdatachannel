/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

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

void test_websocketserver() {
	InitLogger(LogLevel::Debug);

	WebSocketServer::Configuration serverConfig;
	serverConfig.port = 48080;
	serverConfig.enableTls = true;
	// serverConfig.certificatePemFile = ...
	// serverConfig.keyPemFile = ...
	serverConfig.bindAddress = "127.0.0.1"; // to test IPv4 fallback
	serverConfig.maxMessageSize = 1000;     // to test max message size
	WebSocketServer server(std::move(serverConfig));

	shared_ptr<WebSocket> client;
	std::atomic<bool> headersVerified = false;
	server.onClient([&client, &headersVerified](shared_ptr<WebSocket> incoming) {
		cout << "WebSocketServer: Client connection received" << endl;
		client = incoming;

		if(auto addr = client->remoteAddress())
			cout << "WebSocketServer: Client remote address is " << *addr << endl;

		client->onOpen([wclient = make_weak_ptr(client), &headersVerified]() {
			cout << "WebSocketServer: Client connection open" << endl;
			if(auto client = wclient.lock()) {
				if(auto path = client->path())
					cout << "WebSocketServer: Requested path is " << *path << endl;

				// Test custom headers functionality
				auto requestHeadersMultimap = client->requestHeaders();
				std::map<string, string> requestHeaders;
				for (const auto& [key, value] : requestHeadersMultimap) {
					requestHeaders[key] = value;
				}

				cout << "WebSocketServer: Checking custom headers..." << endl;
				bool customHeaderFound = requestHeaders.find("x-test-custom") != requestHeaders.end() &&
				                        requestHeaders.at("x-test-custom") == "test-value-123";
				bool authHeaderFound = requestHeaders.find("authorization") != requestHeaders.end() &&
				                      requestHeaders.at("authorization") == "Bearer custom-token";

				if (customHeaderFound && authHeaderFound) {
					cout << "WebSocketServer: Custom headers verified successfully!" << endl;
					headersVerified = true;
				} else {
					cout << "WebSocketServer: Custom headers verification failed!" << endl;
					cout << "  X-Test-Custom header: " << (customHeaderFound ? "FOUND" : "NOT FOUND") << endl;
					cout << "  Authorization header: " << (authHeaderFound ? "FOUND" : "NOT FOUND") << endl;
				}
			}
		});

		client->onClosed([]() {
			cout << "WebSocketServer: Client connection closed" << endl;
		});

		client->onMessage([wclient = make_weak_ptr(client)](variant<binary, string> message) {
			if(auto client = wclient.lock())
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
		}
		else {
			binary bin = std::move(get<binary>(message));
			if ((maxSizeReceived = (bin.size() == 1000)))
				cout << "WebSocket: Received large message truncated at max size" << endl;
			else
				cout << "WebSocket: Received large message NOT TRUNCATED" << endl;
		}
	});

	ws.open("wss://localhost:48080/", {
		{"X-Test-Custom", "test-value-123"},
		{"Authorization", "Bearer custom-token"}
	});

	int attempts = 15;
	while ((!ws.isOpen() || !received || !headersVerified) && attempts--)
		this_thread::sleep_for(1s);

	if (!ws.isOpen())
		throw runtime_error("WebSocket is not open");

	if (!received || !maxSizeReceived)
		throw runtime_error("Expected messages not received");

	if (!headersVerified)
		throw runtime_error("Custom headers not verified");

	ws.close();
	this_thread::sleep_for(1s);

	server.stop();
	this_thread::sleep_for(1s);

	cout << "Success" << endl;
}

#endif
