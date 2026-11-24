/**
 * Smoke test for prebuilt libdatachannel packages
 *
 * Connects two local peers and exchanges a message over a data channel, which
 * exercises ICE, DTLS (crypto backend) and SCTP from the packaged library.
 */

#include "rtc/rtc.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>

#if !RTC_ENABLE_MEDIA
#error "Prebuilt package is expected to have media support"
#endif
#if !RTC_ENABLE_WEBSOCKET
#error "Prebuilt package is expected to have WebSocket support"
#endif

using namespace std::chrono_literals;

int main() try {
	std::cout << "libdatachannel " << RTC_VERSION << std::endl;
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::Configuration config; // host candidates only, no network access required
	auto pc1 = std::make_shared<rtc::PeerConnection>(config);
	auto pc2 = std::make_shared<rtc::PeerConnection>(config);

	std::weak_ptr<rtc::PeerConnection> wpc1 = pc1, wpc2 = pc2;
	pc1->onLocalDescription([wpc2](rtc::Description sdp) {
		if (auto pc2 = wpc2.lock())
			pc2->setRemoteDescription(std::move(sdp));
	});
	pc1->onLocalCandidate([wpc2](rtc::Candidate candidate) {
		if (auto pc2 = wpc2.lock())
			pc2->addRemoteCandidate(std::move(candidate));
	});
	pc2->onLocalDescription([wpc1](rtc::Description sdp) {
		if (auto pc1 = wpc1.lock())
			pc1->setRemoteDescription(std::move(sdp));
	});
	pc2->onLocalCandidate([wpc1](rtc::Candidate candidate) {
		if (auto pc1 = wpc1.lock())
			pc1->addRemoteCandidate(std::move(candidate));
	});

	std::promise<std::string> received;
	std::shared_ptr<rtc::DataChannel> dc2;
	pc2->onDataChannel([&dc2, &received](std::shared_ptr<rtc::DataChannel> dc) {
		dc2 = dc;
		dc->onMessage([&received](rtc::message_variant message) {
			if (std::holds_alternative<std::string>(message))
				received.set_value(std::get<std::string>(message));
		});
	});

	auto dc1 = pc1->createDataChannel("test");
	dc1->onOpen([&dc1]() { dc1->send("Hello from prebuilt libdatachannel"); });

	auto future = received.get_future();
	if (future.wait_for(30s) != std::future_status::ready) {
		std::cerr << "Timeout: data channel message not received" << std::endl;
		return 1;
	}
	std::cout << "Received: " << future.get() << std::endl;

	dc1->close();
	pc1->close();
	pc2->close();
	dc1.reset();
	dc2.reset();
	pc1.reset();
	pc2.reset();
	rtc::Cleanup().wait_for(10s);

	std::cout << "Success" << std::endl;
	return 0;

} catch (const std::exception &e) {
	std::cerr << "Error: " << e.what() << std::endl;
	return 1;
}
