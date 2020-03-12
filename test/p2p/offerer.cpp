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

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

const char base64_url_alphabet[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'
};

std::string base64_encode(const std::string & in) {
  std::string out;
  int val =0, valb=-6;
  size_t len = in.length();
  unsigned int i = 0;
  for (i = 0; i < len; i++) {
    unsigned char c = in[i];
    val = (val<<8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(base64_url_alphabet[(val>>valb)&0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) {
    out.push_back(base64_url_alphabet[((val<<8)>>(valb+8))&0x3F]);
  }
  return out;
}

std::vector<string> split(const string& str, const string& delim)
{
    vector<string> tokens;
    size_t prev = 0, pos = 0;
    do
    {
        pos = str.find(delim, prev);
        if (pos == string::npos) pos = str.length();
        string token = str.substr(prev, pos-prev);
        if (!token.empty()) tokens.push_back(token);
        prev = pos + delim.length();
    }
    while (pos < str.length() && prev < str.length());
    return tokens;
}

int main(int argc, char **argv) {
	InitLogger(LogLevel::Warning);

	Configuration config;
	// config.iceServers.emplace_back("stun.l.google.com:19302");

	std::string payload;

	auto pc = std::make_shared<PeerConnection>(config);

	pc->onLocalDescription([wpc = make_weak_ptr(pc)](const Description &description){
		auto pc = wpc.lock();
		if (!pc)
			return;

		pc->connectionInfo += description;
		pc->connectionInfo += "xxxxx";
	});

	pc->onLocalCandidate([wpc = make_weak_ptr(pc)](const Candidate &candidate) {
		auto pc = wpc.lock();
		if (!pc)
			return;

		pc->connectionInfo += candidate;

		cout << pc->connectionInfo << endl << endl;

		auto encoded = base64_encode(pc->connectionInfo);
		cout << "http://localhost:8080/answerer.html?connection=" << encoded << endl << endl;

		httplib::Client cli("localhost", 8000);

		auto res = cli.Get("/state/json");
		if (!res)
			return;

		while (res->status == -1) {
			res = cli.Get("/state/json");
		}

		std::string description;
		auto parts = split(res->body, "xxxxx");
		pc->setRemoteDescription(parts[0]);
	});

	pc->onStateChange([wpc = make_weak_ptr(pc)](PeerConnection::State state){ 
		cout << "[State: " << state << "]" << endl;
	});

	pc->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "[Gathering State: " << state << "]" << endl;
	});

	auto dc = pc->createDataChannel("test"); // this is the offerer, so create a data channel

	dc->onOpen([&]() { cout << "[DataChannel open: " << dc->label() << "]" << endl; });

	dc->onClosed([&]() { cout << "[DataChannel closed: " << dc->label() << "]" << endl; });

	dc->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "[Received: " << get<string>(message) << "]" << endl;
		}
	});

	this_thread::sleep_for(1s);

	bool exit = false;
	while (!exit) {
		cout << endl
		     << "**********************************************************************************"
		        "*****"
		     << endl
		     << "* 0: Exit /"
		     << " 1: Enter remote description /"
		     << " 2: Enter remote candidate /"
		     << " 3: Send message *" << endl
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
			if (!dc->isOpen()) {
				cout << "** Channel is not Open ** ";
				break;
			}
			cout << "[Message]: ";
			string message;
			getline(cin, message);
			dc->send(message);
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
