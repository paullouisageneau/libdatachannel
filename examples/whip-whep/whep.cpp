#include <arpa/inet.h>
#include <iostream>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>

#include <httplib.h>

using nlohmann::json;

const int BUFFER_SIZE = 2048;

// ffmpeg -f lavfi -i testsrc=duration=10:size=1280x720:rate=30 test.mp4
// ffmpeg -re -i test.mp4 -c:v libvpx -c:a aac -f rtp udp://127.0.0.1:6000
int main(int argc, char *argv[]) {

    httplib::Server svr;

    rtc::InitLogger(rtc::LogLevel::Debug);

    // Create peer connection
    std::shared_ptr<rtc::PeerConnection> pc = std::make_shared<rtc::PeerConnection>();

    std::shared_ptr<rtc::Track> track;
    const rtc::SSRC ssrc = 42;
    pc->onStateChange(
        [](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });

    pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        std::cout << "Gathering State: " << state << std::endl;
    });
    // Set up onLocalDescription
    pc->onLocalDescription([](rtc::Description description) {});

    // Set up onLocalCandidate
    pc->onLocalCandidate([](rtc::Candidate candidate) {});

    pc->onTrack([&track, &ssrc](std::shared_ptr<rtc::Track> offeredTrack) {
        rtc::Description::Media trackDesc = offeredTrack->description();
        if (trackDesc.direction() == rtc::Description::Direction::RecvOnly)
            return;

        // Find a format we can send
        for (int pt : trackDesc.payloadTypes()) {
            rtc::Description::Media::RtpMap *rtpMap = trackDesc.rtpMap(pt);
            if (rtpMap->format == "H264") {         // for instance
                trackDesc.addSSRC(ssrc, "mycname"); // Add sent SSRC
                offeredTrack->setDescription(std::move(trackDesc));
                std::atomic_store(&track, offeredTrack);
                break;
            }
        }
    });

    std::thread t([&track, &ssrc]() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(6000);

        if (bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("Failed to bind UDP socket on 127.0.0.1:6000");
        }

        int rcvBufSize = 212992;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcvBufSize),
                   sizeof(rcvBufSize));

        // receive from UDP
        char buffer[BUFFER_SIZE];
        int len;
        while ((len = recv(sock, buffer, BUFFER_SIZE, 0)) >= 0) {

            if (len < sizeof(rtc::RtpHeader) || !track->isOpen()) {
                continue;
            }

            rtc::RtpHeader *rtp = reinterpret_cast<rtc::RtpHeader *>(buffer);
            rtp->setSsrc(ssrc);

            track->send(reinterpret_cast<const std::byte *>(buffer), len);
        }
    });

    svr.Options("/whep", [](const httplib::Request &req, httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        res.status = 204; // No Content
    });

    svr.Post("/whep", [&pc](const httplib::Request &req, httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");

        rtc::Description remoteOffer(req.body, "offer");
        pc->setRemoteDescription(remoteOffer);

        std::optional<rtc::Description> description = pc->localDescription();
        res.set_content(std::string(description.value()), "application/sdp");
    });

    std::cout << "Server listening on http://localhost:8080\n";
    svr.listen("0.0.0.0", 8080);

    return 0;
}