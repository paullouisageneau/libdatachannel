/**
 * libdatachannel client-benchmark example
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2019-2021 Murat Dogan
 * Copyright (c) 2020 Will Munn
 * Copyright (c) 2020 Nico Chatzi
 * Copyright (c) 2020 Lara Mackey
 * Copyright (c) 2020 Erik Cota-Robles
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include "parse_cl.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;
using std::shared_ptr;
using std::weak_ptr;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

using nlohmann::json;

std::string localId;
std::unordered_map<std::string, shared_ptr<rtc::PeerConnection>> peerConnectionMap;
std::unordered_map<std::string, shared_ptr<rtc::DataChannel>> dataChannelMap;

shared_ptr<rtc::PeerConnection> createPeerConnection(const rtc::Configuration &config,
                                                     weak_ptr<rtc::WebSocket> wws, std::string id);
std::string randomId(size_t length);

// Benchmark
const size_t messageSize = 65535;
rtc::binary messageData(messageSize);
std::unordered_map<std::string, std::atomic<size_t>> receivedSizeMap;
std::unordered_map<std::string, std::atomic<size_t>> sentSizeMap;
bool noSend = false;

// Benchmark - enableThroughputSet params
bool enableThroughputSet;
int throughtputSetAsKB;
int bufferSize;
const float STEP_COUNT_FOR_1_SEC = 100.0;
const int stepDurationInMs = int(1000 / STEP_COUNT_FOR_1_SEC);

int main(int argc, char **argv) try {
	Cmdline params(argc, argv);

	rtc::InitLogger(rtc::LogLevel::Info);

	// Benchmark - construct message to send
	fill(messageData.begin(), messageData.end(), std::byte(0xFF));

	// Benchmark - enableThroughputSet params
	enableThroughputSet = params.enableThroughputSet();
	throughtputSetAsKB = params.throughtputSetAsKB();
	bufferSize = params.bufferSize();

	// No Send option
	noSend = params.noSend();
	if (noSend)
		std::cout << "Not sending data (one way benchmark)." << std::endl;

	rtc::Configuration config;
	std::string stunServer = "";
	if (params.noStun()) {
		std::cout
		    << "No STUN server is configured. Only local hosts and public IP addresses supported."
		    << std::endl;
	} else {
		if (params.stunServer().substr(0, 5).compare("stun:") != 0) {
			stunServer = "stun:";
		}
		stunServer += params.stunServer() + ":" + std::to_string(params.stunPort());
		std::cout << "STUN server is " << stunServer << std::endl;
		config.iceServers.emplace_back(stunServer);
	}

	localId = randomId(4);
	std::cout << "The local ID is " << localId << std::endl;

	auto ws = std::make_shared<rtc::WebSocket>();

	std::promise<void> wsPromise;
	auto wsFuture = wsPromise.get_future();

	ws->onOpen([&wsPromise]() {
		std::cout << "WebSocket connected, signaling ready" << std::endl;
		wsPromise.set_value();
	});

	ws->onError([&wsPromise](std::string s) {
		std::cout << "WebSocket error" << std::endl;
		wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
	});

	ws->onClosed([]() { std::cout << "WebSocket closed" << std::endl; });

	ws->onMessage([&config, wws = make_weak_ptr(ws)](auto data) {
		if (!std::holds_alternative<std::string>(data))
			return;

		json message = json::parse(std::get<std::string>(data));

		auto it = message.find("id");
		if (it == message.end())
			return;

		auto id = it->get<std::string>();

		it = message.find("type");
		if (it == message.end())
			return;

		auto type = it->get<std::string>();

		shared_ptr<rtc::PeerConnection> pc;
		if (auto jt = peerConnectionMap.find(id); jt != peerConnectionMap.end()) {
			pc = jt->second;
		} else if (type == "offer") {
			std::cout << "Answering to " + id << std::endl;
			pc = createPeerConnection(config, wws, id);
		} else {
			return;
		}

		if (type == "offer" || type == "answer") {
			auto sdp = message["description"].get<std::string>();
			pc->setRemoteDescription(rtc::Description(sdp, type));
		} else if (type == "candidate") {
			auto sdp = message["candidate"].get<std::string>();
			auto mid = message["mid"].get<std::string>();
			pc->addRemoteCandidate(rtc::Candidate(sdp, mid));
		}
	});

	const std::string wsPrefix =
	    params.webSocketServer().find("://") == std::string::npos ? "ws://" : "";
	const std::string url = wsPrefix + params.webSocketServer() + ":" +
	                        std::to_string(params.webSocketPort()) + "/" + localId;

	std::cout << "WebSocket URL is " << url << std::endl;
	ws->open(url);

	std::cout << "Waiting for signaling to be connected..." << std::endl;
	wsFuture.get();

	std::string id;
	std::cout << "Enter a remote ID to send an offer:" << std::endl;
	std::cin >> id;
	std::cin.ignore();
	if (id.empty()) {
		// Nothing to do
		return 0;
	}

	if (id == localId) {
		std::cout << "Invalid remote ID (This is the local ID). Exiting..." << std::endl;
		return 0;
	}

	std::cout << "Offering to " + id << std::endl;
	auto pc = createPeerConnection(config, ws, id);

	// We are the offerer, so create a data channel to initiate the process
	for (int i = 1; i <= params.dataChannelCount(); i++) {
		const std::string label = "DC-" + std::to_string(i);
		std::cout << "Creating DataChannel with label \"" << label << "\"" << std::endl;
		auto dc = pc->createDataChannel(label);
		receivedSizeMap.emplace(label, 0);
		sentSizeMap.emplace(label, 0);

		// Set Buffer Size
		dc->setBufferedAmountLowThreshold(bufferSize);

		dc->onOpen([id, wdc = make_weak_ptr(dc), label]() {
			std::cout << "DataChannel from " << id << " open" << std::endl;
			if (noSend)
				return;

			if (enableThroughputSet)
				return;

			if (auto dcLocked = wdc.lock()) {
				try {
					while (dcLocked->bufferedAmount() <= bufferSize) {
						dcLocked->send(messageData);
						sentSizeMap.at(label) += messageData.size();
					}
				} catch (const std::exception &e) {
					std::cout << "Send failed: " << e.what() << std::endl;
				}
			}
		});

		dc->onBufferedAmountLow([wdc = make_weak_ptr(dc), label]() {
			if (noSend)
				return;

			if (enableThroughputSet)
				return;

			auto dcLocked = wdc.lock();
			if (!dcLocked)
				return;

			// Continue sending
			try {
				while (dcLocked->isOpen() && dcLocked->bufferedAmount() <= bufferSize) {
					dcLocked->send(messageData);
					sentSizeMap.at(label) += messageData.size();
				}
			} catch (const std::exception &e) {
				std::cout << "Send failed: " << e.what() << std::endl;
			}
		});

		dc->onClosed([id]() { std::cout << "DataChannel from " << id << " closed" << std::endl; });

		dc->onMessage([id, wdc = make_weak_ptr(dc), label](auto data) {
			if (std::holds_alternative<rtc::binary>(data))
				receivedSizeMap.at(label) += std::get<rtc::binary>(data).size();
		});

		dataChannelMap.emplace(label, dc);
	}

	const int duration = params.durationInSec() > 0 ? params.durationInSec() : INT32_MAX;
	std::cout << "Benchmark will run for " << duration << " seconds" << std::endl;

	int printCounter = 0;
	int printStatCounter = 0;
	steady_clock::time_point printTime = steady_clock::now();
	steady_clock::time_point stepTime = steady_clock::now();
	// Byte count to send for every loop
	int byteToSendOnEveryLoop = throughtputSetAsKB * stepDurationInMs;
	for (int i = 1; i <= duration * STEP_COUNT_FOR_1_SEC; ++i) {
		std::this_thread::sleep_for(milliseconds(stepDurationInMs));
		printCounter++;

		if (enableThroughputSet) {
			const double elapsedTimeInSecs =
			    std::chrono::duration<double>(steady_clock::now() - stepTime).count();

			stepTime = steady_clock::now();

			int byteToSendThisLoop = static_cast<int>(
			    byteToSendOnEveryLoop * ((elapsedTimeInSecs * 1000.0) / stepDurationInMs));

			rtc::binary tempMessageData(byteToSendThisLoop);
			fill(tempMessageData.begin(), tempMessageData.end(), std::byte(0xFF));

			for (const auto &[label, dc] : dataChannelMap) {
				if (dc->isOpen() && dc->bufferedAmount() <= bufferSize * byteToSendOnEveryLoop) {
					dc->send(tempMessageData);
					sentSizeMap.at(label) += tempMessageData.size();
				}
			}
		}

		if (printCounter >= STEP_COUNT_FOR_1_SEC) {
			const double elapsedTimeInSecs =
			    std::chrono::duration<double>(steady_clock::now() - printTime).count();
			printTime = steady_clock::now();

			unsigned long receiveSpeedTotal = 0;
			unsigned long sendSpeedTotal = 0;
			std::cout << "#" << i / STEP_COUNT_FOR_1_SEC << std::endl;
			for (const auto &[label, dc] : dataChannelMap) {
				unsigned long channelReceiveSpeed = static_cast<int>(
				    receivedSizeMap[label].exchange(0) / (elapsedTimeInSecs * 1000));
				unsigned long channelSendSpeed =
				    static_cast<int>(sentSizeMap[label].exchange(0) / (elapsedTimeInSecs * 1000));

				std::cout << std::setw(10) << label << " Received: " << channelReceiveSpeed
				          << " KB/s"
				          << "   Sent: " << channelSendSpeed << " KB/s"
				          << "   BufferSize: " << dc->bufferedAmount() << std::endl;

				receiveSpeedTotal += channelReceiveSpeed;
				sendSpeedTotal += channelSendSpeed;
			}
			std::cout << std::setw(10) << "TOTAL"
			          << " Received: " << receiveSpeedTotal << " KB/s"
			          << "   Sent: " << sendSpeedTotal << " KB/s" << std::endl;

			printStatCounter++;
			printCounter = 0;
		}

		if (printStatCounter >= 5) {
			std::cout << "Stats# "
			          << "Received Total: " << pc->bytesReceived() / (1000 * 1000) << " MB"
			          << "   Sent Total: " << pc->bytesSent() / (1000 * 1000) << " MB"
			          << "   RTT: " << pc->rtt().value_or(0ms).count() << " ms" << std::endl;
			std::cout << std::endl;
			printStatCounter = 0;
		}
	}

	std::cout << "Cleaning up..." << std::endl;

	dataChannelMap.clear();
	peerConnectionMap.clear();
	receivedSizeMap.clear();
	sentSizeMap.clear();
	return 0;

} catch (const std::exception &e) {
	std::cout << "Error: " << e.what() << std::endl;
	dataChannelMap.clear();
	peerConnectionMap.clear();
	receivedSizeMap.clear();
	sentSizeMap.clear();
	return -1;
}

// Create and setup a PeerConnection
shared_ptr<rtc::PeerConnection> createPeerConnection(const rtc::Configuration &config,
                                                     weak_ptr<rtc::WebSocket> wws, std::string id) {
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	pc->onStateChange(
	    [](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });

	pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
		std::cout << "Gathering State: " << state << std::endl;
	});

	pc->onLocalDescription([wws, id](rtc::Description description) {
		json message = {{"id", id},
		                {"type", description.typeString()},
		                {"description", std::string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate([wws, id](rtc::Candidate candidate) {
		json message = {{"id", id},
		                {"type", "candidate"},
		                {"candidate", std::string(candidate)},
		                {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onDataChannel([id](shared_ptr<rtc::DataChannel> dc) {
		const std::string label = dc->label();
		std::cout << "DataChannel from " << id << " received with label \"" << label << "\""
		          << std::endl;

		std::cout << "###########################################" << std::endl;
		std::cout << "### Check other peer's screen for stats ###" << std::endl;
		std::cout << "###########################################" << std::endl;

		receivedSizeMap.emplace(dc->label(), 0);
		sentSizeMap.emplace(dc->label(), 0);

		// Set Buffer Size
		dc->setBufferedAmountLowThreshold(bufferSize);

		if (!noSend && !enableThroughputSet) {
			try {
				while (dc->bufferedAmount() <= bufferSize) {
					dc->send(messageData);
					sentSizeMap.at(label) += messageData.size();
				}
			} catch (const std::exception &e) {
				std::cout << "Send failed: " << e.what() << std::endl;
			}
		}

		if (!noSend && enableThroughputSet) {
			// Create Send Data Thread
			// Thread will join when data channel destroyed or closed
			std::thread([wdc = make_weak_ptr(dc), label]() {
				steady_clock::time_point stepTime = steady_clock::now();
				// Byte count to send for every loop
				int byteToSendOnEveryLoop = throughtputSetAsKB * stepDurationInMs;
				while (true) {
					std::this_thread::sleep_for(milliseconds(stepDurationInMs));

					auto dcLocked = wdc.lock();
					if (!dcLocked)
						break;

					if (!dcLocked->isOpen())
						break;

					try {
						const double elapsedTimeInSecs =
						    std::chrono::duration<double>(steady_clock::now() - stepTime).count();

						stepTime = steady_clock::now();

						int byteToSendThisLoop =
						    static_cast<int>(byteToSendOnEveryLoop *
						                     ((elapsedTimeInSecs * 1000.0) / stepDurationInMs));

						rtc::binary tempMessageData(byteToSendThisLoop);
						fill(tempMessageData.begin(), tempMessageData.end(), std::byte(0xFF));

						if (dcLocked->bufferedAmount() <= bufferSize) {
							dcLocked->send(tempMessageData);
							sentSizeMap.at(label) += tempMessageData.size();
						}
					} catch (const std::exception &e) {
						std::cout << "Send failed: " << e.what() << std::endl;
					}
				}
				std::cout << "Send Data Thread exiting..." << std::endl;
			}).detach();
		}

		dc->onBufferedAmountLow([wdc = make_weak_ptr(dc), label]() {
			if (noSend)
				return;

			if (enableThroughputSet)
				return;

			auto dcLocked = wdc.lock();
			if (!dcLocked)
				return;

			// Continue sending
			try {
				while (dcLocked->isOpen() && dcLocked->bufferedAmount() <= bufferSize) {
					dcLocked->send(messageData);
					sentSizeMap.at(label) += messageData.size();
				}
			} catch (const std::exception &e) {
				std::cout << "Send failed: " << e.what() << std::endl;
			}
		});

		dc->onClosed([id]() { std::cout << "DataChannel from " << id << " closed" << std::endl; });

		dc->onMessage([id, wdc = make_weak_ptr(dc), label](auto data) {
			if (std::holds_alternative<rtc::binary>(data))
				receivedSizeMap.at(label) += std::get<rtc::binary>(data).size();
		});

		dataChannelMap.emplace(label, dc);
	});

	peerConnectionMap.emplace(id, pc);
	return pc;
};

// Helper function to generate a random ID
std::string randomId(size_t length) {
	using std::chrono::high_resolution_clock;
	static thread_local std::mt19937 rng(
	    static_cast<unsigned int>(high_resolution_clock::now().time_since_epoch().count()));
	static const std::string characters(
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
	std::string id(length, '0');
	std::uniform_int_distribution<int> uniform(0, int(characters.size() - 1));
	std::generate(id.begin(), id.end(), [&]() { return characters.at(uniform(rng)); });
	return id;
}
