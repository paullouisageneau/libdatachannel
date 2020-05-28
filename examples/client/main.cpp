/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2019 Murat Dogan
 * Copyright (c) 2020 Will Munn
 * Copyright (c) 2020 Lara Mackey
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "rtc/rtc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

unordered_map<string, shared_ptr<PeerConnection>> peerConnectionMap;
unordered_map<string, shared_ptr<DataChannel>> dataChannelMap;

string localId;

shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id);
string randomId(size_t length);

int main(int argc, char **argv) {
	rtc::InitLogger(LogLevel::Warning);

	Configuration config;
	config.iceServers.emplace_back("stun.l.google.com:19302"); // change to your STUN server

	localId = randomId(8);
	cout << "The local ID is: " << localId << endl;

	auto ws = make_shared<WebSocket>();

	ws->onOpen([]() { cout << "WebSocket connected, signaling ready" << endl; });

	ws->onClosed([]() { cout << "WebSocket closed" << endl; });

	ws->onError([](const string &error) { cout << "WebSocket failed: " << error << endl; });

	ws->onMessage([&](const variant<binary, string> &data) {
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
			auto str = message["description"].get<string>();
			pc->setRemoteDescription(Description(str, type));
		} else if (type == "candidate") {
			auto str = message["candidate"].get<string>();
			auto mid = message["mid"].get<string>();
			pc->addRemoteCandidate(Candidate(str, mid));
		}
	});

	const string url = "ws://localhost:8000/" + localId;
	ws->open(url);

	cout << "Waiting for signaling to be connected..." << endl;
	while (!ws->isOpen()) {
		if (ws->isClosed())
			return 1;
		this_thread::sleep_for(100ms);
	}

	while (true) {
		string id;
		cout << "Enter a remote ID to send an offer:" << endl;
		cin >> id;
		cin.ignore();
		if (id.empty())
			break;
		if (id == localId)
			continue;

		cout << "Offering to " + id << endl;
		auto pc = createPeerConnection(config, ws, id);

		// We are the offerer, so create a data channel to initiate the process
		const string label = "test";
		cout << "Creating DataChannel with label \"" << label << "\"" << endl;
		auto dc = pc->createDataChannel(label);

		dc->onOpen([id, wdc = make_weak_ptr(dc)]() {
			cout << "DataChannel from " << id << " open" << endl;
			if (auto dc = wdc.lock())
				dc->send("Hello from " + localId);
		});

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dc->onMessage([id](const variant<binary, string> &message) {
			if (!holds_alternative<string>(message))
				return;

			cout << "Message from " << id << " received: " << get<string>(message) << endl;
		});

		dataChannelMap.emplace(id, dc);

		this_thread::sleep_for(1s);
	}

	cout << "Cleaning up..." << endl;

	dataChannelMap.clear();
	peerConnectionMap.clear();
	return 0;
}

// Create and setup a PeerConnection
shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id) {
	auto pc = make_shared<PeerConnection>(config);

	pc->onStateChange([](PeerConnection::State state) { cout << "State: " << state << endl; });

	pc->onGatheringStateChange(
	    [](PeerConnection::GatheringState state) { cout << "Gathering State: " << state << endl; });

	pc->onLocalDescription([wws, id](const Description &description) {
		json message = {
		    {"id", id}, {"type", description.typeString()}, {"description", string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate([wws, id](const Candidate &candidate) {
		json message = {{"id", id},
		                {"type", "candidate"},
		                {"candidate", string(candidate)},
		                {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onDataChannel([id](shared_ptr<DataChannel> dc) {
		cout << "DataChannel from " << id << " received with label \"" << dc->label() << "\""
		     << endl;

		dc->send("Hello from " + localId);

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dc->onMessage([id](const variant<binary, string> &message) {
			if (!holds_alternative<string>(message))
				return;

			cout << "Message from " << id << " received: " << get<string>(message) << endl;
		});

		dataChannelMap.emplace(id, dc);
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
	uniform_int_distribution<int> dist(0, characters.size() - 1);
	generate(id.begin(), id.end(), [&]() { return characters.at(dist(rng)); });
	return id;
}
