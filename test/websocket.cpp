/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

void test_websocket() {
	InitLogger(LogLevel::Debug);

	const string myMessage = "Hello world from libdatachannel";

	WebSocket::Configuration config;
	config.disableTlsVerification = true;
	WebSocket ws(std::move(config));

	ws.onOpen([&ws, &myMessage]() {
		cout << "WebSocket: Open" << endl;
		ws.send(myMessage);
	});

	ws.onError([](string error) { cout << "WebSocket: Error: " << error << endl; });

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

	ws.open("wss://echo.websocket.org:443/");

	int attempts = 20;
	while ((!ws.isOpen() || !received) && attempts--)
		this_thread::sleep_for(1s);

	if (!ws.isOpen())
		throw runtime_error("WebSocket is not open");

	if (!received)
		throw runtime_error("Expected message not received");

	ws.close();
	this_thread::sleep_for(1s);

	cout << "Success" << endl;
}

#endif
