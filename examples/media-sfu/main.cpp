/**
 * libdatachannel client example
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
#include <vector>

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

		track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

		const rtc::SSRC targetSSRC = 42;
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

		pc->setLocalDescription();

		// Set the sender's answer
		std::cout << "Please copy/paste the answer provided by the SENDER: " << std::endl;
		std::string sdp;
		std::getline(std::cin, sdp);
		std::cout << "Got answer" << sdp << std::endl;
		json j = json::parse(sdp);
		rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
		pc->setRemoteDescription(answer);

		// For each receiver
		while (true) {
			auto r = std::make_shared<Receiver>();
			r->conn = std::make_shared<rtc::PeerConnection>();
			r->conn->onStateChange([](rtc::PeerConnection::State state) {
				std::cout << "State: " << state << std::endl;
			});
			r->conn->onGatheringStateChange([r](rtc::PeerConnection::GatheringState state) {
				std::cout << "Gathering State: " << state << std::endl;
				if (state == rtc::PeerConnection::GatheringState::Complete) {
					auto description = r->conn->localDescription();
					json message = {{"type", description->typeString()},
					                {"sdp", std::string(description.value())}};
					std::cout << "Please copy/paste this offer to the RECEIVER: " << message
					          << std::endl;
				}
			});
			rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
			media.addH264Codec(96);
			media.setBitrate(3000);
			media.addSSRC(targetSSRC, "video-send");

			r->track = r->conn->addTrack(media);

			r->track->onOpen([r]() {
				r->track->requestKeyframe(); // So the receiver can start playing immediately
			});
			r->track->onMessage([](rtc::binary var) {}, nullptr);

			r->conn->setLocalDescription();

			std::cout << "Please copy/paste the answer provided by the RECEIVER: " << std::endl;
			std::string sdp;
			std::getline(std::cin, sdp);
			std::cout << "Got answer" << sdp << std::endl;
			json j = json::parse(sdp);
			rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
			r->conn->setRemoteDescription(answer);

			receivers.push_back(r);
		}

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}
}
