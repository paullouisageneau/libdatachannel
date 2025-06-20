#include <iostream>
#include <string.h>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>

#include <httplib.h>

using nlohmann::json;

int main(int argc, char *argv[]) {

	httplib::Server svr;

	rtc::InitLogger(rtc::LogLevel::Debug);

	// Create peer connection
	std::shared_ptr<rtc::PeerConnection> pc = std::make_shared<rtc::PeerConnection>();

	std::shared_ptr<rtc::RtcpReceivingSession> session =
	    std::make_shared<rtc::RtcpReceivingSession>();

	std::shared_ptr<rtc::Track> track;
	pc->onStateChange(
	    [](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });

	pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
		std::cout << "Gathering State: " << state << std::endl;
	});
	// Set up onLocalDescription
	pc->onLocalDescription([](rtc::Description description) {});

	// Set up onLocalCandidate
	pc->onLocalCandidate([](rtc::Candidate candidate) {});

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(5000);

	pc->onTrack([&track, &pc, &session, &sock, &addr](std::shared_ptr<rtc::Track> offeredTrack) {
		rtc::Description::Media trackDesc = offeredTrack->description();

		// We only want the sendrec track
		if (trackDesc.direction() != rtc::Description::Direction::SendRecv)
			return;

		for (int pt : trackDesc.payloadTypes()) {
			rtc::Description::Media::RtpMap *rtpMap = trackDesc.rtpMap(pt);
			if (rtpMap->format == "VP8") {
				offeredTrack->setDescription(std::move(trackDesc));

				std::atomic_store(&track, offeredTrack);

				// filter out rtcp
				track->setMediaHandler(session);

				// forward rtp data
				track->onMessage(
				    [&sock, &addr](rtc::binary message) {
					    rtc::RtpHeader *rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());

					    rtp->setPayloadType(96); // match paylod type in ffmpeg sdp
					    ssize_t sent =
					        sendto(sock, reinterpret_cast<const char *>(message.data()),
					               int(message.size()), 0,
					               reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
				    },
				    nullptr);
				break;
			}
		}
	});

	svr.Options("/whip", [](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Access-Control-Allow-Origin", "*");
		res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
		res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
		res.status = 204; // No Content
	});

	svr.Post("/whip", [&pc](const httplib::Request &req, httplib::Response &res) {
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