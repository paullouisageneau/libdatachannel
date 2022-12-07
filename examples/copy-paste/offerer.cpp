/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 * Copyright (c) 2019 Murat Dogan
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using std::shared_ptr;
using std::weak_ptr;
template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

int main(int argc, char **argv) {
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::Configuration config;
	// config.iceServers.emplace_back("stun.l.google.com:19302");

	auto pc = std::make_shared<rtc::PeerConnection>(config);

	pc->onLocalDescription([](rtc::Description description) {
		std::cout << "Local Description (Paste this to the other peer):" << std::endl;
		std::cout << std::string(description) << std::endl;
	});

	pc->onLocalCandidate([](rtc::Candidate candidate) {
		std::cout << "Local Candidate (Paste this to the other peer after the local description):"
		          << std::endl;
		std::cout << std::string(candidate) << std::endl << std::endl;
	});

	pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[State: " << state << "]" << std::endl;
	});

	pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
		std::cout << "[Gathering State: " << state << "]" << std::endl;
	});

	auto dc = pc->createDataChannel("test"); // this is the offerer, so create a data channel

	dc->onOpen([&]() { std::cout << "[DataChannel open: " << dc->label() << "]" << std::endl; });

	dc->onClosed(
	    [&]() { std::cout << "[DataChannel closed: " << dc->label() << "]" << std::endl; });

	dc->onMessage([](auto data) {
		if (std::holds_alternative<std::string>(data)) {
			std::cout << "[Received: " << std::get<std::string>(data) << "]" << std::endl;
		}
	});

	std::this_thread::sleep_for(1s);

	bool exit = false;
	while (!exit) {
		std::cout
		    << std::endl
		    << "**********************************************************************************"
		       "*****"
		    << std::endl
		    << "* 0: Exit /"
		    << " 1: Enter remote description /"
		    << " 2: Enter remote candidate /"
		    << " 3: Send message /"
		    << " 4: Print Connection Info *" << std::endl
		    << "[Command]: ";

		int command = -1;
		std::cin >> command;
		std::cin.ignore();

		switch (command) {
		case 0: {
			exit = true;
			break;
		}
		case 1: {
			// Parse Description
			std::cout << "[Description]: ";
			std::string sdp, line;
			while (getline(std::cin, line) && !line.empty()) {
				sdp += line;
				sdp += "\r\n";
			}
			pc->setRemoteDescription(sdp);
			break;
		}
		case 2: {
			// Parse Candidate
			std::cout << "[Candidate]: ";
			std::string candidate;
			getline(std::cin, candidate);
			pc->addRemoteCandidate(candidate);
			break;
		}
		case 3: {
			// Send Message
			if (!dc->isOpen()) {
				std::cout << "** Channel is not Open ** ";
				break;
			}
			std::cout << "[Message]: ";
			std::string message;
			getline(std::cin, message);
			dc->send(message);
			break;
		}
		case 4: {
			// Connection Info
			if (!dc || !dc->isOpen()) {
				std::cout << "** Channel is not Open ** ";
				break;
			}
			rtc::Candidate local, remote;
			std::optional<std::chrono::milliseconds> rtt = pc->rtt();
			if (pc->getSelectedCandidatePair(&local, &remote)) {
				std::cout << "Local: " << local << std::endl;
				std::cout << "Remote: " << remote << std::endl;
				std::cout << "Bytes Sent:" << pc->bytesSent()
				          << " / Bytes Received:" << pc->bytesReceived() << " / Round-Trip Time:";
				if (rtt.has_value())
					std::cout << rtt.value().count();
				else
					std::cout << "null";
				std::cout << " ms";
			} else {
				std::cout << "Could not get Candidate Pair Info" << std::endl;
			}
			break;
		}
		default: {
			std::cout << "** Invalid Command **" << std::endl;
			break;
		}
		}
	}

	if (dc)
		dc->close();

	if (pc)
		pc->close();
}
