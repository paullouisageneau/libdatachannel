/**
 * Copyright (c) 2019 Paul-Louis Ageneau, Murat Dogan
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
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

int main(int argc, char **argv) {
	rtc::InitLogger(LogLevel::Warning);

	Configuration config;
	// config.iceServers.emplace_back("stun.l.google.com:19302");

	auto pc = std::make_shared<PeerConnection>(config);

	pc->onLocalDescription([](Description description) {
		cout << "Local Description (Paste this to the other peer):" << endl;
		cout << string(description) << endl;
	});

	pc->onLocalCandidate([](Candidate candidate) {
		cout << "Local Candidate (Paste this to the other peer after the local description):"
		     << endl;
		cout << string(candidate) << endl << endl;
	});

	pc->onStateChange(
	    [](PeerConnection::State state) { cout << "[State: " << state << "]" << endl; });
	pc->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "[Gathering State: " << state << "]" << endl;
	});

	shared_ptr<DataChannel> dc = nullptr;
	pc->onDataChannel([&](shared_ptr<DataChannel> _dc) {
		cout << "[Got a DataChannel with label: " << _dc->label() << "]" << endl;
		dc = _dc;

		dc->onClosed([&]() { cout << "[DataChannel closed: " << dc->label() << "]" << endl; });

		dc->onMessage([](variant<binary, string> message) {
			if (holds_alternative<string>(message)) {
				cout << "[Received message: " << get<string>(message) << "]" << endl;
			}
		});
	});

	bool exit = false;
	while (!exit) {
		cout << endl
		     << "**********************************************************************************"
		        "*****"
		     << endl
		     << "* 0: Exit /"
		     << " 1: Enter remote description /"
		     << " 2: Enter remote candidate /"
		     << " 3: Send message /"
		     << " 4: Print Connection Info *" << endl
		     << "[Command]: ";

		int command = -1;
		cin >> command;
		cin.ignore();

		switch (command) {
		case 0: {
			exit = true;
			break;
		}
		case 1: {
			// Parse Description
			cout << "[Description]: ";
			string sdp, line;
			while (getline(cin, line) && !line.empty()) {
				sdp += line;
				sdp += "\r\n";
			}
			std::cout << sdp;
			pc->setRemoteDescription(sdp);
			break;
		}
		case 2: {
			// Parse Candidate
			cout << "[Candidate]: ";
			string candidate;
			getline(cin, candidate);
			pc->addRemoteCandidate(candidate);
			break;
		}
		case 3: {
			// Send Message
			if (!dc || !dc->isOpen()) {
				cout << "** Channel is not Open ** ";
				break;
			}
			cout << "[Message]: ";
			string message;
			getline(cin, message);
			dc->send(message);
			break;
		}
		case 4: {
			// Connection Info
			if (!dc || !dc->isOpen()) {
				cout << "** Channel is not Open ** ";
				break;
			}
			Candidate local, remote;
			std::optional<std::chrono::milliseconds> rtt = pc->rtt();
			if (pc->getSelectedCandidatePair(&local, &remote)) {
				cout << "Local: " << local << endl;
				cout << "Remote: " << remote << endl;
				cout << "Bytes Sent:" << pc->bytesSent()
				     << " / Bytes Received:" << pc->bytesReceived() << " / Round-Trip Time:";
				if (rtt.has_value())
					cout << rtt.value().count();
				else
					cout << "null";
				cout << " ms";
			} else
				cout << "Could not get Candidate Pair Info" << endl;
			break;
		}
		default: {
			cout << "** Invalid Command ** ";
			break;
		}
		}
	}

	if (dc)
		dc->close();
	if (pc)
		pc->close();
}
