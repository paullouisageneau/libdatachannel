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

    pc->onStateChange([](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });

    pc->onGatheringStateChange( [pc](rtc::PeerConnection::GatheringState state) {
        std::cout << "Gathering State: " << state << std::endl;
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto description = pc->localDescription();
            json message = {
                    {"type", description->typeString()}, {"sdp", std::string(description.value())}
            };
            std::cout << message << std::endl;
        }
    });

    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(5000);
    addr.sin_family = AF_INET;

    RTCPSession session;
    session.setOutgoingCallback([&pc](const rtc::message_ptr& ptr) {
        // There is a chance we could send an RTCP packet before we connect (i.e. REMB)
        if (pc->state() == rtc::PeerConnection::State::Connected) {
            pc->sendMedia(ptr);
        }
    });
//    session.requestBitrate(4000000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)
    pc->onMedia([&session, &sock_fd, &addr](const rtc::message_ptr& ptr) {
        auto resp = session.onData(ptr);
        if (resp.has_value()) {
            // This is an RTP packet
            sendto(sock_fd, resp.value()->data(), resp.value()->size(), 0, (const struct sockaddr*) &addr, sizeof(addr));
        }
    });

    rtc::Description offer;
    rtc::Description::Media &m = offer.addVideoMedia(rtc::Description::RecvOnly);
    m.addH264Codec(96);
    m.setBitrate(3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)
//    m.setBitrate(5000000);
    pc->setLocalDescription(offer);
    auto dc = pc->createDataChannel("test");

    std::cout << "Expect RTP video traffic on localhost:5000" << std::endl;
    std::cout << "Please copy/paste the answer provided by the browser: " << std::endl;
    std::string sdp;
    std::getline (std::cin,sdp);
    std::cout << "Got answer" << sdp << std::endl;
    json j = json::parse(sdp);
    rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
    pc->setRemoteDescription(answer, answer);
    std::cout << "Press any key to exit." << std::endl;
    std::cin >> sdp;
}