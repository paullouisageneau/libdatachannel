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

static std::unordered_map<std::string, std::shared_ptr<Client>> clients{};

auto threadPool = DispatchQueue("Main", 1);

const std::string signaling_serverip = "127.0.0.1";
const int signaling_serverport = 8000;

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
			std::cout << "state: ,peer: " << state << id << std::endl;

			if (isDisconnectedState(state)) {
				// remove disconnected client
				threadPool.dispatch([id]() {
					clients.erase(id);
				});
        	}
		}
	);

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

	pc->setLocalDescription();

	return client;
}

void handleOffer(
	const std::string& id,
	const rtc::Configuration& config,
	const std::shared_ptr<rtc::WebSocket> ws) {

	// create peerconnection
	clients.emplace(id, createPeerConnection(id, config, make_weak_ptr(ws)));
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

	if (type == "offer") {
		// TODO handle offer
		handleOffer(id, config, ws);

		// create peer connection

		// gen local desc

		// gen answer

		// send answer
	} else if (type == "answer") {
		// TODO
	} else if (type == "leave") {
		// TODO
	} else if (type == "userbusy") {
		// TODO
	} else if (type == "useroffline") {
		// TODO
	} else {
		std::cout << "unknown message type: " << type << std::endl;
	}
}

int main(int argc, char **argv) {
	std::cout << "hello world" << std::endl;

	if (argc < 2) {
		std::cerr << "Client id must be specified" << std::endl;
		return 1;
	}

	auto config = rtc::Configuration();
	config.disableAutoNegotiation = true;
	// not setting stun server for now
	// auto stunServer = std::string("stun:stun.l.google.com:19302");
	// config.iceServers.emplace_back(stunServer);

	// parse the client id from the cmd line
	auto peerid = std::string(argv[1]);

	std::cout << "Client id is: " << peerid << std::endl;

	const auto localid = std::string("gjw");

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

	std::cout << "Waiting for signaling to be connected..." << std::endl;

	while (!ws->isOpen()) {
        if (ws->isClosed())
            return 1;
        std::this_thread::sleep_for(100ms);
    }

	return 0;
}