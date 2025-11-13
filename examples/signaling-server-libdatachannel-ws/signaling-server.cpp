/**
 * signaling server example for libdatachannel
 * Copyright (c) 2024 Tim Schneider
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>
using nlohmann::json;


using namespace std::chrono_literals;
using std::shared_ptr;
using std::weak_ptr;
template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

std::string get_user(weak_ptr<rtc::WebSocket> wws) {
	const auto &ws = wws.lock();
	const auto path_ = ws->path().value();
	const auto user = path_.substr(path_.rfind('/') + 1);
	return user;
}

void signalHandler(int signum) {
	std::cout << "Interrupt signal (" << signum << ") received.\n";
	// terminate program
	exit(signum);
}

int main(int argc, char *argv[]) {
	rtc::WebSocketServerConfiguration config;
	config.port = 8000;
	config.enableTls = false;
	config.certificatePemFile = std::nullopt;
	config.keyPemFile = std::nullopt;
	config.keyPemPass = std::nullopt;
	config.bindAddress = std::nullopt;
	config.connectionTimeout = std::nullopt;

	// Check command line arguments.
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			const size_t len = strlen(argv[0]);
			char *path = (char *)malloc(len + 1);
			strcpy(path, argv[0]);
			path[len] = 0;

			char *app_name = NULL;
			// app_name = last_path_segment(path, "\\/");
			fprintf(stderr,
			        "Usage: %s [-p <port>] [-a <bind-address>] [--connection-timeout <timeout>] "
			        "[--enable-tls] [--certificatePemFile <file>] [--keyPemFile <keyPemFile>] "
			        "[--keyPemPass <pass>]\n"
			        "Example:\n"
			        "    %s -p 8000 -a 127.0.0.1 \n",
			        app_name, app_name);
			free(path);
			return EXIT_FAILURE;
		}
		if (strcmp(argv[i], "-p") == 0) {
			config.port = atoi(argv[++i]);
			continue;
		}
		if (strcmp(argv[i], "-a") == 0) {
			config.bindAddress = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--connection-timeout") == 0) {
			config.connectionTimeout = std::chrono::milliseconds(atoi(argv[++i]));
			continue;
		}
		if (strcmp(argv[i], "--enable-tls") == 0) {
			config.enableTls = true;
			continue;
		}
		if (strcmp(argv[i], "--certificatePemFile") == 0) {
			config.certificatePemFile = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--keyPemFile") == 0) {
			config.keyPemFile = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--keyPemPass") == 0) {
			config.keyPemPass = argv[++i];
			continue;
		}
	}

	auto wss = std::make_shared<rtc::WebSocketServer>(config);
	std::unordered_map<std::string, std::shared_ptr<rtc::WebSocket>> clients_map;

	wss->onClient([&clients_map](std::shared_ptr<rtc::WebSocket> ws) {
		std::promise<void> wsPromise;
		auto wsFuture = wsPromise.get_future();
		std::cout << "WebSocket client (remote-address: " << ws->remoteAddress().value() << ")"
		          << std::endl;

		ws->onOpen([&clients_map, &wsPromise, wws = make_weak_ptr(ws)]() {
			const auto user = get_user(wws);
			std::cout << "WebSocket connected (user: " << user << ")" << std::endl;
			clients_map.insert_or_assign(user, wws.lock());
			wsPromise.set_value();
		});
		ws->onError([&clients_map, &wsPromise, wws = make_weak_ptr(ws)](std::string s) {
			wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
			const auto user = get_user(wws);
			std::cout << "WebSocket error (user: " << user << ")" << std::endl;
			clients_map.erase(user);
		});
		ws->onClosed([&clients_map, &wsPromise, wws = make_weak_ptr(ws)]() {
			const auto user = get_user(wws);
			std::cout << "WebSocket closed (user: " << user << ")" << std::endl;
			clients_map.erase(user);
		});
		ws->onMessage([&clients_map, wws = make_weak_ptr(ws)](auto data) {
			// data holds either std::string or rtc::binary
			if (!std::holds_alternative<std::string>(data))
				return;

			json message = json::parse(std::get<std::string>(data));

			auto it = message.find("id");
			if (it == message.end())
				return;

			auto id = it->get<std::string>();

			auto client_dst = clients_map.find(id);
			if (client_dst == clients_map.end()) {
				std::cout << "not found" << std::endl;
			} else {
				const auto user = get_user(wws);

				message["id"] = user;
				auto &[id_dst, ws_dst] = *client_dst;
				std::cout << user << "->" << id << ": " << message.dump() << std::endl;
				ws_dst->send(message.dump());
			}
		});
		std::cout << "Waiting for client to be connected..." << std::endl;
		wsFuture.get();
	});

	signal(SIGINT, signalHandler);
	while (true) {
		std::this_thread::sleep_for(1s);
	}

	return EXIT_SUCCESS;
}