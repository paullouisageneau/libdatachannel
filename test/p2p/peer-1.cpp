/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#include <chrono>
#include <iostream>
#include <memory>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

int main(int argc, char **argv) {
	rtcInitLogger(LogLevel::debug);
	rtc::Configuration config;

	// config.iceServers.emplace_back("stun.l.google.com:19302");
	// config.enableIceTcp = true;

	// Add TURN Server Example
	// IceServer turnServer("TURN_SERVER_URL", "PORT_NO", "USERNAME", "PASSWORD",
	//							IceServer::RelayType::TurnTls);
	// config.iceServers.push_back(turnServer);

	auto pc = std::make_shared<PeerConnection>(config);

	pc->onLocalDescription([](const Description &sdp) {
		std::string s(sdp);
		std::replace(s.begin(), s.end(), '\n', static_cast<char>(94));
		cout << "Local Description (Paste this to other peer):" << endl << s << endl << endl;
	});

	pc->onLocalCandidate([](const Candidate &candidate) {
		cout << "Local Candidate (Paste this to other peer):" << endl << candidate << endl << endl;
	});

	pc->onStateChange(
	    [](PeerConnection::State state) { cout << "[ State: " << state << " ]" << endl; });

	pc->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "[ Gathering State: " << state << " ]" << endl;
	});

	auto dc = pc->createDataChannel("test");
	dc->onOpen([&]() {
		if (!dc)
			return;
		cout << "[ DataChannel open: " << dc->label() << " ]" << endl;
	});

	dc->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "[ Received: " << get<string>(message) << " ]" << endl;
		}
	});

	bool exit = false;
	while (!exit) {
		cout << endl
		     << endl
		     << "*************************************************************************" << endl
		     << "* 0: Exit /"
		     << " 1: Enter Description /"
		     << " 2: Enter Candidate /"
		     << " 3: Send Message *" << endl
		     << " [Command]: ";

		int command;
		std::string sdp, candidate, message;
		const char *a;
		std::unique_ptr<Candidate> candidatePtr;
		std::unique_ptr<Description> descPtr;
		cin >> command;

		switch (command) {
		case 0:
			exit = true;
			break;

		case 1:
			// Parse Description
			cout << "[SDP]: ";
			sdp = "";
			while (sdp.length() == 0)
				getline(cin, sdp);

			std::replace(sdp.begin(), sdp.end(), static_cast<char>(94), '\n');
			descPtr = std::make_unique<Description>(sdp);
			pc->setRemoteDescription(*descPtr);
			break;

		case 2:
			// Parse Candidate
			cout << "[Candidate]: ";
			candidate = "";
			while (candidate.length() == 0)
				getline(cin, candidate);

			candidatePtr = std::make_unique<Candidate>(candidate);
			pc->addRemoteCandidate(*candidatePtr);
			break;

		case 3:
			// Send Message
			if (!dc->isOpen()) {
				cout << "** Channel is not Open ** ";
				break;
			}
			cout << "[Message]: ";
			message = "";
			while (message.length() == 0)
				getline(cin, message);
			dc->send(message);
			break;

		default:
			cout << "** Invalid Command ** ";
			break;
		}
	}

	if (dc)
		dc->close();
	if (pc)
		pc->close();
}
