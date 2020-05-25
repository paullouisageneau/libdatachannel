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

#include <iostream>
#include <memory>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

int main(int argc, char **argv) {
	rtc::InitLogger(LogLevel::Warning);

	Configuration config;
	config.iceServers.emplace_back("stun.l.google.com:19302"); // change to your STUN server

	auto pc = std::make_shared<PeerConnection>(config);

	const std::string url = "ws://localhost:8000/";

	auto ws = std::make_shared<WebSocket>(url);
	ws->onOpen([]() {

	});
	ws->onMessage([wpc = make_weak_ptr(pc)](const std::variant<binary, string> &data) {
		auto pc = wpc.lock();
		if (pc && holds_alternative<string>(data)) {
			json message = json::parse(get<string>(data));
			if (auto it = message.find("type"); it != message.end()) {
				auto type = it->get<string>();
				if (type == "offer" || type == "answer") {
					auto str = message["description"].get<string>();
					pc->setRemoteDescription(Description(str, type));
				} else if (type == "candidate") {
					auto str = message["candidate"].get<string>();
					auto mid = message["mid"].get<string>();
					pc->addRemoteCandidate(Candidate(str, mid));
				}
			}
		}
	});
	ws->onClosed([]() {

	});

	pc->onLocalDescription([wws = make_weak_ptr(ws)](const Description &description) {
		json message = {{"type", description.typeString()},
		                {"description", std::string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});
	pc->onLocalCandidate([wws = make_weak_ptr(ws)](const Candidate &candidate) {
		json message = {
		    {"type", "candidate"}, {"candidate", std::string(candidate)}, {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});
	pc->onStateChange([wpc = make_weak_ptr(pc)](PeerConnection::State state) {
		cout << "State: " << state << endl;
	});
	pc->onGatheringStateChange(
	    [](PeerConnection::GatheringState state) { cout << "Gathering State: " << state << endl; });

	auto dc = pc->createDataChannel("test"); // this is the offerer, so create a data channel

	dc->onOpen([&]() { cout << "DataChannel open: " << dc->label() << endl; });

	dc->onClosed([&]() { cout << "DataChannel closed: " << dc->label() << endl; });

	dc->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Received: " << get<string>(message) << endl;
		}
	});

	// TODO

	if (dc)
		dc->close();
	if (pc)
		pc->close();
}
