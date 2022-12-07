/**
 * libdatachannel media receiver example
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <iostream>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int SOCKET;
#endif

using nlohmann::json;

int main() {
	try {
		rtc::InitLogger(rtc::LogLevel::Debug);
		auto pc = std::make_shared<rtc::PeerConnection>();

		pc->onStateChange(
		    [](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });

		pc->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
			std::cout << "Gathering State: " << state << std::endl;
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				auto description = pc->localDescription();
				json message = {{"type", description->typeString()},
				                {"sdp", std::string(description.value())}};
				std::cout << message << std::endl;
			}
		});

		SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = inet_addr("127.0.0.1");
		addr.sin_port = htons(5000);

		rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
		media.addH264Codec(96);
		media.setBitrate(
		    3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)

		auto track = pc->addTrack(media);

		auto session = std::make_shared<rtc::RtcpReceivingSession>();
		track->setMediaHandler(session);

		track->onMessage(
		    [session, sock, addr](rtc::binary message) {
			    // This is an RTP packet
			    sendto(sock, reinterpret_cast<const char *>(message.data()), int(message.size()), 0,
			           reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
		    },
		    nullptr);

		pc->setLocalDescription();

		std::cout << "Expect RTP video traffic on localhost:5000" << std::endl;
		std::cout << "Please copy/paste the answer provided by the browser: " << std::endl;
		std::string sdp;
		std::getline(std::cin, sdp);

		std::cout << "Got answer" << sdp << std::endl;
		json j = json::parse(sdp);
		rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
		pc->setRemoteDescription(answer);

		std::cout << "Press any key to exit." << std::endl;
		char dummy;
		std::cin >> dummy;

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}
}
