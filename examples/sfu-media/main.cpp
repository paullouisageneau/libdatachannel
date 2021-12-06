/*
 * libdatachannel client example
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

#include <nlohmann/json.hpp>

using nlohmann::json;

struct Receiver {
	std::shared_ptr<rtc::PeerConnection> conn;
	std::shared_ptr<rtc::Track> track;
};
int main() {
	std::vector<std::shared_ptr<Receiver>> receivers;

	try {
		rtc::InitLogger(rtc::LogLevel::Info);

		auto pc = std::make_shared<rtc::PeerConnection>();
		pc->onStateChange(
		    [](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });
		pc->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
			std::cout << "Gathering State: " << state << std::endl;
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				auto description = pc->localDescription();
				json message = {{"type", description->typeString()},
				                {"sdp", std::string(description.value())}};
				std::cout << "Please copy/paste this offer to the SENDER: " << message << std::endl;
			}
		});

		rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
		media.addH264Codec(96);
		media.setBitrate(
		    3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)

		auto track = pc->addTrack(media);
		pc->setLocalDescription();

		auto session = std::make_shared<rtc::RtcpReceivingSession>();
		track->setMediaHandler(session);

		const rtc::SSRC targetSSRC = 4;

		track->onMessage(
		    [&receivers, targetSSRC](rtc::binary message) {
			    // This is an RTP packet
			    auto rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());
			    rtp->setSsrc(targetSSRC);
			    for (auto pc : receivers) {
				    if (pc->track != nullptr && pc->track->isOpen()) {
					    pc->track->send(message);
				    }
			    }
		    },
		    nullptr);

		// Set the SENDERS Answer
		{
			std::cout << "Please copy/paste the answer provided by the SENDER: " << std::endl;
			std::string sdp;
			std::getline(std::cin, sdp);
			std::cout << "Got answer" << sdp << std::endl;
			json j = json::parse(sdp);
			rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
			pc->setRemoteDescription(answer);
		}

		// For each receiver
		while (true) {
			auto pc = std::make_shared<Receiver>();
			pc->conn = std::make_shared<rtc::PeerConnection>();
			pc->conn->onStateChange([](rtc::PeerConnection::State state) {
				std::cout << "State: " << state << std::endl;
			});
			pc->conn->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
				std::cout << "Gathering State: " << state << std::endl;
				if (state == rtc::PeerConnection::GatheringState::Complete) {
					auto description = pc->conn->localDescription();
					json message = {{"type", description->typeString()},
					                {"sdp", std::string(description.value())}};
					std::cout << "Please copy/paste this offer to the RECEIVER: " << message
					          << std::endl;
				}
			});
			rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
			media.addH264Codec(96);
			media.setBitrate(
			    3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)

			media.addSSRC(targetSSRC, "video-send");

			pc->track = pc->conn->addTrack(media);
			pc->conn->setLocalDescription();

			pc->track->onMessage([](rtc::binary var) {}, nullptr);

			std::cout << "Please copy/paste the answer provided by the RECEIVER: " << std::endl;
			std::string sdp;
			std::getline(std::cin, sdp);
			std::cout << "Got answer" << sdp << std::endl;
			json j = json::parse(sdp);
			rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
			pc->conn->setRemoteDescription(answer);

			receivers.push_back(pc);
		}

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}
}
