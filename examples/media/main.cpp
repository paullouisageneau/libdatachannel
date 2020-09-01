#include <iostream>
#include <memory>
#include <rtc/log.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtp.hpp>

#include <nlohmann/json.hpp>
#include <utility>
#include <arpa/inet.h>

using nlohmann::json;

int main() {
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

	int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in addr;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(5000);
	addr.sin_family = AF_INET;

	rtc::Description::VideoMedia media(rtc::Description::RecvOnly);
	media.addH264Codec(96);
	media.setBitrate(
	    3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)

	auto track = pc->createTrack(media);
	auto dc = pc->createDataChannel("test");

	auto session = std::make_shared<rtc::RtcpSession>();
	track->setRtcpHandler(session);

	track->onMessage(
	    [&session, &sock_fd, &addr](rtc::binary message) {
		    // This is an RTP packet
		    sendto(sock_fd, message.data(), message.size(), 0,
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
}
