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
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

void test_websocketserver() {
	InitLogger(LogLevel::Debug);

	const string myMessage = "Hello world from client";

	WebSocketServer::Configuration serverConfig;
	serverConfig.port = 48080;
	serverConfig.enableTls = true;
	// serverConfig.certificatePemFile = ...
	// serverConfig.keyPemFile = ...
	serverConfig.bindAddress = "127.0.0.1"; // to test IPv4 fallback
	WebSocketServer server(std::move(serverConfig));

	shared_ptr<WebSocket> client;
	server.onClient([&client](shared_ptr<WebSocket> incoming) {
		cout << "WebSocketServer: Client connection received" << endl;
		client = incoming;

		if(auto addr = client->remoteAddress())
			cout << "WebSocketServer: Client remote address is " << *addr << endl;

		client->onOpen([wclient = make_weak_ptr(client)]() {
			cout << "WebSocketServer: Client connection open" << endl;
			if(auto client = wclient.lock())
				if(auto path = client->path())
					cout << "WebSocketServer: Requested path is " << *path << endl;
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

	ws.onOpen([&ws, &myMessage]() {
		cout << "WebSocket: Open" << endl;
		ws.send(myMessage);
	});

	ws.onClosed([]() { cout << "WebSocket: Closed" << endl; });

	std::atomic<bool> received = false;
	ws.onMessage([&received, &myMessage](variant<binary, string> message) {
		if (holds_alternative<string>(message)) {
			string str = std::move(get<string>(message));
			if ((received = (str == myMessage)))
				cout << "WebSocket: Received expected message" << endl;
			else
				cout << "WebSocket: Received UNEXPECTED message" << endl;
		}
	});

	ws.open("wss://localhost:48080/");

	int attempts = 15;
	while ((!ws.isOpen() || !received) && attempts--)
		this_thread::sleep_for(1s);

	if (!ws.isOpen())
		throw runtime_error("WebSocket is not open");

	if (!received)
		throw runtime_error("Expected message not received");

	ws.close();
	this_thread::sleep_for(1s);

	server.stop();
	this_thread::sleep_for(1s);

	cout << "Success" << endl;
}

#endif
