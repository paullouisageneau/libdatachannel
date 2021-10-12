/*
 * libdatachannel media example
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "rtc/rtc.hpp"

#include <iostream>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
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
		sockaddr_in addr;
		addr.sin_addr.s_addr = inet_addr("127.0.0.1");
		addr.sin_port = htons(5000);
		addr.sin_family = AF_INET;

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
		std::cin >> sdp;

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}
}
