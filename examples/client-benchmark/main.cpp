/*
 * libdatachannel client-benchmark example
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2019-2021 Murat Dogan
 * Copyright (c) 2020 Will Munn
 * Copyright (c) 2020 Nico Chatzi
 * Copyright (c) 2020 Lara Mackey
 * Copyright (c) 2020 Erik Cota-Robles
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
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

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using chrono::milliseconds;
using chrono::steady_clock;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

unordered_map<string, shared_ptr<PeerConnection>> peerConnectionMap;
unordered_map<string, shared_ptr<DataChannel>> dataChannelMap;

string localId;

shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id);
string randomId(size_t length);

// Benchmark
const size_t messageSize = 65535;
binary messageData(messageSize);
unordered_map<string, atomic<size_t>> receivedSizeMap;
unordered_map<string, atomic<size_t>> sentSizeMap;
bool noSend = false;

// Benchmark - enableThroughputSet params
bool enableThroughputSet;
int throughtputSetAsKB;
int bufferSize;
const float STEP_COUNT_FOR_1_SEC = 100.0;
const int stepDurationInMs = int(1000 / STEP_COUNT_FOR_1_SEC);

int main(int argc, char **argv) try {
	Cmdline params(argc, argv);

	rtc::InitLogger(LogLevel::Info);

	// Benchmark - construct message to send
	fill(messageData.begin(), messageData.end(), std::byte(0xFF));

	// Benchmark - enableThroughputSet params
	enableThroughputSet = params.enableThroughputSet();
	throughtputSetAsKB = params.throughtputSetAsKB();
	bufferSize = params.bufferSize();

	// No Send option
	noSend = params.noSend();
	if (noSend)
		cout << "Not Sending data. (One way benchmark)." << endl;

	Configuration config;
	string stunServer = "";
	if (params.noStun()) {
		cout << "No STUN server is configured. Only local hosts and public IP addresses supported."
		     << endl;
	} else {
		if (params.stunServer().substr(0, 5).compare("stun:") != 0) {
			stunServer = "stun:";
		}
		stunServer += params.stunServer() + ":" + to_string(params.stunPort());
		cout << "Stun server is " << stunServer << endl;
		config.iceServers.emplace_back(stunServer);
	}

	localId = randomId(4);
	cout << "The local ID is: " << localId << endl;

	auto ws = make_shared<WebSocket>();

	std::promise<void> wsPromise;
	auto wsFuture = wsPromise.get_future();

	ws->onOpen([&wsPromise]() {
		cout << "WebSocket connected, signaling ready" << endl;
		wsPromise.set_value();
	});

	ws->onError([&wsPromise](string s) {
		cout << "WebSocket error" << endl;
		wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
	});

	ws->onClosed([]() { cout << "WebSocket closed" << endl; });

	ws->onMessage([&](variant<binary, string> data) {
		if (!holds_alternative<string>(data))
			return;

		json message = json::parse(get<string>(data));

		auto it = message.find("id");
		if (it == message.end())
			return;
		string id = it->get<string>();

		it = message.find("type");
		if (it == message.end())
			return;
		string type = it->get<string>();

		shared_ptr<PeerConnection> pc;
		if (auto jt = peerConnectionMap.find(id); jt != peerConnectionMap.end()) {
			pc = jt->second;
		} else if (type == "offer") {
			cout << "Answering to " + id << endl;
			pc = createPeerConnection(config, ws, id);
		} else {
			return;
		}

		if (type == "offer" || type == "answer") {
			auto sdp = message["description"].get<string>();
			pc->setRemoteDescription(Description(sdp, type));
		} else if (type == "candidate") {
			auto sdp = message["candidate"].get<string>();
			auto mid = message["mid"].get<string>();
			pc->addRemoteCandidate(Candidate(sdp, mid));
		}
	});

	string wsPrefix = "";
	if (params.webSocketServer().substr(0, 5).compare("ws://") != 0) {
		wsPrefix = "ws://";
	}
	const string url = wsPrefix + params.webSocketServer() + ":" +
	                   to_string(params.webSocketPort()) + "/" + localId;
	cout << "Url is " << url << endl;
	ws->open(url);

	cout << "Waiting for signaling to be connected..." << endl;
	wsFuture.get();

	string id;
	cout << "Enter a remote ID to send an offer:" << endl;
	cin >> id;
	cin.ignore();
	if (id.empty()) {
		// Nothing to do
		return 0;
	}
	if (id == localId) {
		cout << "Invalid remote ID (This is my local ID). Exiting..." << endl;
		return 0;
	}

	cout << "Offering to " + id << endl;
	auto pc = createPeerConnection(config, ws, id);

	// We are the offerer, so create a data channel to initiate the process
	for (int i = 1; i <= params.dataChannelCount(); i++) {
		const string label = "DC-" + std::to_string(i);
		cout << "Creating DataChannel with label \"" << label << "\"" << endl;
		auto dc = pc->createDataChannel(label);
		receivedSizeMap.emplace(label, 0);
		sentSizeMap.emplace(label, 0);

		// Set Buffer Size
		dc->setBufferedAmountLowThreshold(bufferSize);

		dc->onOpen([id, wdc = make_weak_ptr(dc), label]() {
			cout << "DataChannel from " << id << " open" << endl;
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

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dc->onMessage([id, wdc = make_weak_ptr(dc), label](variant<binary, string> data) {
			if (holds_alternative<binary>(data))
				receivedSizeMap.at(label) += get<binary>(data).size();
		});

		dataChannelMap.emplace(label, dc);
	}

	const int duration = params.durationInSec() > 0 ? params.durationInSec() : INT32_MAX;
	cout << "Benchmark will run for " << duration << " seconds" << endl;

	int printCounter = 0;
	int printStatCounter = 0;
	steady_clock::time_point printTime = steady_clock::now();
	steady_clock::time_point stepTime = steady_clock::now();
	// Byte count to send for every loop
	int byteToSendOnEveryLoop = throughtputSetAsKB * stepDurationInMs;
	for (int i = 1; i <= duration * STEP_COUNT_FOR_1_SEC; ++i) {
		this_thread::sleep_for(milliseconds(stepDurationInMs));
		printCounter++;

		if (enableThroughputSet) {
			const double elapsedTimeInSecs =
			    std::chrono::duration<double>(steady_clock::now() - stepTime).count();

			stepTime = steady_clock::now();

			int byteToSendThisLoop = static_cast<int>(
			    byteToSendOnEveryLoop * ((elapsedTimeInSecs * 1000.0) / stepDurationInMs));

			binary tempMessageData(byteToSendThisLoop);
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
			cout << "#" << i / STEP_COUNT_FOR_1_SEC << endl;
			for (const auto &[label, dc] : dataChannelMap) {
				unsigned long channelReceiveSpeed = static_cast<int>(
				    receivedSizeMap[label].exchange(0) / (elapsedTimeInSecs * 1000));
				unsigned long channelSendSpeed =
				    static_cast<int>(sentSizeMap[label].exchange(0) / (elapsedTimeInSecs * 1000));

				cout << std::setw(10) << label << " Received: " << channelReceiveSpeed << " KB/s"
				     << "   Sent: " << channelSendSpeed << " KB/s"
				     << "   BufferSize: " << dc->bufferedAmount() << endl;

				receiveSpeedTotal += channelReceiveSpeed;
				sendSpeedTotal += channelSendSpeed;
			}
			cout << std::setw(10) << "TOTL"
			     << " Received: " << receiveSpeedTotal << " KB/s"
			     << "   Sent: " << sendSpeedTotal << " KB/s" << endl;

			printStatCounter++;
			printCounter = 0;
		}

		if (printStatCounter >= 5) {
			cout << "Stats# "
			     << "Received Total: " << pc->bytesReceived() / (1000 * 1000) << " MB"
			     << "   Sent Total: " << pc->bytesSent() / (1000 * 1000) << " MB"
			     << "   RTT: " << pc->rtt().value_or(0ms).count() << " ms" << endl;
			cout << endl;
			printStatCounter = 0;
		}
	}

	cout << "Cleaning up..." << endl;

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
shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id) {
	auto pc = make_shared<PeerConnection>(config);

	pc->onStateChange([](PeerConnection::State state) { cout << "State: " << state << endl; });

	pc->onGatheringStateChange(
	    [](PeerConnection::GatheringState state) { cout << "Gathering State: " << state << endl; });

	pc->onLocalDescription([wws, id](Description description) {
		json message = {
		    {"id", id}, {"type", description.typeString()}, {"description", string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate([wws, id](Candidate candidate) {
		json message = {{"id", id},
		                {"type", "candidate"},
		                {"candidate", string(candidate)},
		                {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onDataChannel([id](shared_ptr<DataChannel> dc) {
		const string label = dc->label();
		cout << "DataChannel from " << id << " received with label \"" << label << "\"" << endl;

		cout << "###########################################" << endl;
		cout << "### Check other peer's screen for stats ###" << endl;
		cout << "###########################################" << endl;

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
					this_thread::sleep_for(milliseconds(stepDurationInMs));

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

						binary tempMessageData(byteToSendThisLoop);
						fill(tempMessageData.begin(), tempMessageData.end(), std::byte(0xFF));

						if (dcLocked->bufferedAmount() <= bufferSize) {
							dcLocked->send(tempMessageData);
							sentSizeMap.at(label) += tempMessageData.size();
						}
					} catch (const std::exception &e) {
						std::cout << "Send failed: " << e.what() << std::endl;
					}
				}
				cout << "Send Data Thread exiting..." << endl;
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

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dc->onMessage([id, wdc = make_weak_ptr(dc), label](variant<binary, string> data) {
			if (holds_alternative<binary>(data))
				receivedSizeMap.at(label) += get<binary>(data).size();
		});

		dataChannelMap.emplace(label, dc);
	});

	peerConnectionMap.emplace(id, pc);
	return pc;
};

// Helper function to generate a random ID
string randomId(size_t length) {
	static const string characters(
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
	string id(length, '0');
	default_random_engine rng(random_device{}());
	uniform_int_distribution<int> dist(0, int(characters.size() - 1));
	generate(id.begin(), id.end(), [&]() { return characters.at(dist(rng)); });
	return id;
}
