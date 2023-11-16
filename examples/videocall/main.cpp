#include <iostream>
#include <variant>
#include <shared_mutex>
#include <memory>
#include <thread>
#include <chrono>
#include <rtc/rtc.hpp>
#include <rtc/websocket.hpp>
#include <rtc/common.hpp>
#include <rtc/configuration.hpp>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "dispatchqueue.hpp"
#include "helpers.hpp"

using namespace std::chrono_literals;
using json = nlohmann::json;

template <class T> std::weak_ptr<T>
make_weak_ptr(std::shared_ptr<T> ptr) {
	return ptr;
}

static std::unordered_map<std::string, std::shared_ptr<Client>> peerConnMap{};

auto threadPool = DispatchQueue("Main", 1);

const std::string signaling_serverip = "10.196.28.10";
const int signaling_serverport = 8888;

static inline bool isDisconnectedState(const rtc::PeerConnection::State& state) {
	return state == rtc::PeerConnection::State::Disconnected ||
        state == rtc::PeerConnection::State::Failed ||
        state == rtc::PeerConnection::State::Closed;
}

std::shared_ptr<Client> createPeerConnection(
    const std::string& id,
	const rtc::Configuration &config,
    std::weak_ptr<rtc::WebSocket> wws) {

	// create and setup PeerConnection

	auto pc = std::make_shared<rtc::PeerConnection>(config);
    auto client = std::make_shared<Client>(pc);

	pc->onStateChange(
		[id](rtc::PeerConnection::State state) {
			std::cout << "state: " << state << ", " << "peer: " << id << std::endl;

			if (isDisconnectedState(state)) {
				// remove disconnected client
				threadPool.dispatch([id]() {
					peerConnMap.erase(id);
				});
        	}
		}
	);

	pc->onLocalDescription(
		[wws, id](rtc::Description description) {
		json message = {{"id", id},
		                {"type", description.typeString()},
		                {"description", std::string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate(
		[wws, id](rtc::Candidate candidate) {
		json message = {{"id", id},
		                {"type", "candidate"},
		                {"candidate", std::string(candidate)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onGatheringStateChange(
        [wpc = make_weak_ptr(pc), id, wws](rtc::PeerConnection::GatheringState state) {
        std::cout << "Gathering State: " << state << std::endl;
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            if(auto pc = wpc.lock()) {
                auto description = pc->localDescription();

				json message = {
                    {"id", id},
                    {"type", description->typeString()},
                    {"sdp", std::string(description.value())}
                };

				// Gathering complete, send answer
                if (auto ws = wws.lock()) {
                    ws->send(message.dump());
                } else {
					std::cout << "owner ship to websocket is expired" << std::endl;
				}
            } else {
				std::cout << "owner ship to peerconn is expired" << std::endl;
			}
        }
    });

	// TODO(Jiawei): add video and audio
	rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
	media.addH264Codec(96);
	media.setBitrate(
		3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)

	auto track = pc->addTrack(media);

	// pc->setLocalDescription();

	return client;
}

void handleOffer(
	const std::string& id,
	const rtc::Configuration& config,
	const std::shared_ptr<rtc::WebSocket> ws) {

	std::cout << "Got offer request answering to " + id << std::endl;

	// create peerconnection
	peerConnMap.emplace(id, createPeerConnection(id, config, make_weak_ptr(ws)));
}

void handleWSMsg(
	const json message,
	const rtc::Configuration config,
	const std::shared_ptr<rtc::WebSocket> ws) {

	// the id field indicates which peer we are connecting to
	auto it = message.find("id");

	if (it == message.end()) {
		std::cout << "id field not found" << std::endl;
		return;
	}

	auto id = it->get<std::string>();

	it = message.find("type");

	if (it == message.end()) {
		std::cout << "type field not found" << std::endl;
		return;
	}

	auto type = it->get<std::string>();

	auto peer = std::shared_ptr<Client>();

	if (auto jt = peerConnMap.find(id); jt != peerConnMap.end()) {
		peer = jt->second;
	} else if (type == "offer") {
		handleOffer(id, config, ws);
		peer = peerConnMap[id];
	} else {
		return;
	}

	if (type == "offer" || type == "answer") {
		auto sdp = message["sdp"].get<std::string>();
		peer->peerConnection->setRemoteDescription(rtc::Description(sdp, type));
		/* now create the answer */
		peer->peerConnection->setLocalDescription();
	} else if (type == "candidate") {
		/* FIXME: avoid nested objects! */
		auto candidates = message["candidate"]["candidate"].get<std::string>();
		peer->peerConnection->addRemoteCandidate(rtc::Candidate(candidates, "0")); // 0 for now
	} else if (type == "leave") {
		// TODO
		std::cout << "connection failed due to: " << type << std::endl;
	} else if (type == "userbusy") {
		// TODO
		std::cout << "connection failed due to: " << type << std::endl;
	} else if (type == "useroffline") {
		// TODO
		std::cout << "connection failed due to: " << type << std::endl;
	} else {
		std::cout << "unknown message type: " << type << std::endl;
	}
}

int main(int argc, char **argv) try {
	std::cout << "hello world" << std::endl;

	if (argc < 2) {
		std::cerr << "Client id must be specified" << std::endl;
		return 1;
	}

	auto config = rtc::Configuration();
	config.disableAutoNegotiation = true;
	// not setting stun server for now
	auto stunServer = std::string("stun:stun.l.google.com:19302");
	config.iceServers.emplace_back(stunServer);

	// parse the client id from the cmd line
	auto localid = std::string(argv[1]);

	std::cout << "Client id is: " << localid << std::endl;

	// open connection to the signal server using websockets
	auto ws = std::make_shared<rtc::WebSocket>();

	// register all the handlers here for handling the websocket
	ws->onOpen(
		[]() {
			std::cout << "connected to the signal server via websocket" << std::endl;
		}
	);

	ws->onClosed(
		[]() {
			std::cout << "websocket closed" << std::endl;
		}
	);

	ws->onError(
		[](const std::string& error) {
			std::cout << "failed to connect the signal server due to: " << error << std::endl;
		}
	);

	ws->onMessage(
		[&config, &ws](std::variant<rtc::binary, std::string> data) {
			if (!std::holds_alternative<std::string>(data))
            	return;

			auto message = json::parse(std::get<std::string>(data));

			// dispatch the messge to the threadpool for handling

			threadPool.dispatch(
				[message, config, ws]() {
					handleWSMsg(message, config, ws);
				}
			);
		}
	);

	// initiate connection with the signaling server
	const std::string url = "ws://"
		+ signaling_serverip
		+ ":"
		+ std::to_string(signaling_serverport)
		+ "/"
		+ "join/"
		+ localid;

	std::cout << "the signaling server url is: " << url << std::endl;

	// connect to the singaling server
	ws->open(url);

	std::cout << "waiting for signaling to be connected..." << std::endl;

	while (!ws->isOpen()) {
        if (ws->isClosed()) {
			std::cerr << "Failed to connect to the signal server" << std::endl;
            return 1;
		}
        std::this_thread::sleep_for(100ms);
    }

	auto quit = false;

	while (!quit) {
        std::string command;
        std::cout << "Enter quit or q to exit" << std::endl;
        std::cin >> command;
        std::cin.ignore();

		if (command == "quit" || command == "q") {
        	std::cout << "exiting" << std::endl;
        	quit = true;
		} else if (command == "connect") {
			std::string peerid;
			std::cin >> peerid;
			std::cout << "connecting to " << peerid << std::endl;
			handleOffer(peerid, config, ws);
		}
    }

    std::cout << "Cleaning up..." << std::endl;

	return 0;
} catch (const std::exception &e) {
	std::cerr << "Error: " << e.what() << std::endl;
	return 1;
}