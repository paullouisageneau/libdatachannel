/*
 * WHIP/WHEP signaling example.
 *
 * A single HTTP server that serves the example page and both signaling
 * endpoints:
 *
 *   GET    /              the page in index.html
 *   POST   /whip          ingest  -- a peer publishes video to us
 *   POST   /whep          egress  -- a peer subscribes to video from us
 *   DELETE /whip/<id>     tear the ingest session down
 *   DELETE /whep/<id>     tear the egress session down
 *
 * RTP from any source is forwarded to every sink, so the example can be driven
 * three ways: browser to browser with no external tooling, browser to gstreamer
 * (record what is published), or gstreamer to browser (play out a file).
 *
 *   sources: the WHIP peer's track, and RTP arriving on UDP 6000
 *   sinks:   the WHEP peer's track, and RTP sent to UDP 5000
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <rtc/rtc.hpp>

#include <httplib.h>

namespace {

// The only codec this example handles. Everything else is stripped from the
// answers, so a peer cannot pick a format we are unable to forward.
const std::string videoCodec = "VP8";

const int defaultHttpPort = 8080;

const char *localHost = "127.0.0.1";
const uint16_t udpEgressPort = 5000;  // we send RTP here
const uint16_t udpIngressPort = 6000; // we receive RTP here

// The payload type the gstreamer pipelines in README.md expect.
const int udpPayloadType = 96;

const rtc::SSRC egressSsrc = 42;

const int bufferSize = 2048;
const auto gatheringTimeout = std::chrono::seconds(10);

// A signaling session, created by POST and destroyed by DELETE.
struct Session {
	std::string id;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::RtcpReceivingSession> rtcpSession; // ingest only
	std::promise<void> gatheringDone;
	std::shared_future<void> gatheringDoneFuture;
	std::once_flag gatheringFlag;

	// Written from a libdatachannel callback, read while forwarding RTP.
	std::mutex mediaMutex;
	std::shared_ptr<rtc::Track> track;
	int payloadType = udpPayloadType;
};

// One publisher and one subscriber at a time. The identifier still matters even
// with a single slot: it is what Location names, and DELETE has to tell a live
// session from one already torn down.
struct Slot {
	const char *name;
	std::mutex mutex;
	std::shared_ptr<Session> active;
};

Slot ingestSlot{"whip", {}, nullptr};
Slot egressSlot{"whep", {}, nullptr};

int egressSocket = -1;
struct sockaddr_in egressAddr = {};

std::string randomResourceId() {
	static thread_local std::mt19937 rng{std::random_device{}()};
	static const char *hex = "0123456789abcdef";
	std::string id(16, '0');
	for (char &c : id) {
		c = hex[rng() % 16];
	}
	return id;
}

bool isFinished(rtc::PeerConnection::State state) {
	return state == rtc::PeerConnection::State::Disconnected ||
	       state == rtc::PeerConnection::State::Failed ||
	       state == rtc::PeerConnection::State::Closed;
}

void setCorsHeaders(httplib::Response &res, const char *methods) {
	res.set_header("Access-Control-Allow-Origin", "*");
	res.set_header("Access-Control-Allow-Methods", methods);
	res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
	// The page is same-origin so it would not need this, but a third-party WHIP
	// client does: without it a browser cannot read Location on a cross-origin
	// response, and can never delete the session it just created.
	res.set_header("Access-Control-Expose-Headers", "Location");
}

// Detach a session and close it outside the lock. Closing a PeerConnection can
// wait on its callback threads, and those must never find the mutex held.
void dropSession(Slot &slot, const std::shared_ptr<Session> &session) {
	std::shared_ptr<Session> dead;
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		if (slot.active == session) {
			dead = std::move(slot.active);
		}
	}
	if (dead) {
		dead->pc->close();
	}
}

// Send one RTP packet to every sink. The buffer is rewritten in place between
// sinks because each one wants a different payload type, so this must not be
// called concurrently on the same buffer -- each caller owns its own.
void broadcastRtp(std::byte *data, size_t size) {
	if (size < sizeof(rtc::RtpHeader)) {
		return;
	}

	auto *rtp = reinterpret_cast<rtc::RtpHeader *>(data);

	if (egressSocket >= 0) {
		rtp->setPayloadType(udpPayloadType);
		if (sendto(egressSocket, reinterpret_cast<const char *>(data), int(size), 0,
		           reinterpret_cast<const struct sockaddr *>(&egressAddr),
		           sizeof(egressAddr)) < 0) {
			std::cerr << "Failed to forward RTP to UDP" << std::endl;
		}
	}

	std::shared_ptr<rtc::Track> track;
	int payloadType = udpPayloadType;
	{
		std::lock_guard<std::mutex> lock(egressSlot.mutex);
		if (egressSlot.active) {
			std::lock_guard<std::mutex> mediaLock(egressSlot.active->mediaMutex);
			track = egressSlot.active->track;
			payloadType = egressSlot.active->payloadType;
		}
	}

	// Null until a subscriber has negotiated a track, which is every packet that
	// arrives before the first one connects.
	if (!track || !track->isOpen()) {
		return;
	}

	rtp->setSsrc(egressSsrc);
	rtp->setPayloadType(payloadType);

	try {
		track->send(data, size);
	} catch (const std::exception &e) {
		std::cerr << "Failed to send RTP to subscriber: " << e.what() << std::endl;
	}
}

// Receive RTP on the ingress port and hand it to the same fan-out the WHIP peer
// feeds. Nothing is buffered: with no sink attached the packets are dropped.
void receiveRtpFromUdp() {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		std::cerr << "Failed to create UDP ingress socket" << std::endl;
		return;
	}

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(localHost);
	addr.sin_port = htons(udpIngressPort);

	if (bind(sock, reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr)) < 0) {
		std::cerr << "Failed to bind UDP socket on " << localHost << ":" << udpIngressPort
		          << std::endl;
		return;
	}

	int rcvBufSize = 212992;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcvBufSize),
	           sizeof(rcvBufSize));

	char buffer[bufferSize];
	int len;
	while ((len = recv(sock, buffer, bufferSize, 0)) >= 0) {
		broadcastRtp(reinterpret_cast<std::byte *>(buffer), size_t(len));
	}
}

// Keep only the codec we handle. Selecting a payload type without removing the
// others leaves them all in the answer, so the peer stays free to use something
// else and the negotiation only works by luck.
//
// Collect first and remove second: removeRtpMap() also drops any RTX map that
// referenced the payload type it removed, and rtpMap() throws rather than
// returning null for one that is already gone, so removing while iterating
// would throw straight out of the callback.
bool keepOnlyVideoCodec(rtc::Description::Media &desc) {
	std::vector<int> unwanted;
	for (int pt : desc.payloadTypes()) {
		if (desc.rtpMap(pt)->format != videoCodec) {
			unwanted.push_back(pt);
		}
	}

	for (int pt : unwanted) {
		desc.removeRtpMap(pt);
	}

	return !desc.payloadTypes().empty();
}

void onIngestTrack(const std::shared_ptr<Session> &session,
                   std::shared_ptr<rtc::Track> offeredTrack) {
	rtc::Description::Media desc = offeredTrack->description();

	// We only want the sendrecv track
	if (desc.direction() != rtc::Description::Direction::SendRecv) {
		return;
	}

	if (!keepOnlyVideoCodec(desc)) {
		std::cerr << "Publisher does not offer " << videoCodec << ", ignoring track" << std::endl;
		return;
	}

	offeredTrack->setDescription(std::move(desc));

	// filter out rtcp
	offeredTrack->setMediaHandler(session->rtcpSession);

	offeredTrack->onMessage(
	    [](rtc::binary message) { broadcastRtp(message.data(), message.size()); }, nullptr);

	std::lock_guard<std::mutex> lock(session->mediaMutex);
	session->track = offeredTrack;
}

void onEgressTrack(const std::shared_ptr<Session> &session,
                   std::shared_ptr<rtc::Track> offeredTrack) {
	rtc::Description::Media desc = offeredTrack->description();

	// The peer offered recvonly, so libdatachannel hands us the reciprocated
	// sendonly track. Anything still recvonly is not ours to send on.
	if (desc.direction() == rtc::Description::Direction::RecvOnly) {
		return;
	}

	if (!keepOnlyVideoCodec(desc)) {
		std::cerr << "Subscriber does not accept " << videoCodec << ", ignoring track" << std::endl;
		return;
	}

	const int payloadType = desc.payloadTypes().front();
	desc.addSSRC(egressSsrc, "mycname"); // Add sent SSRC
	offeredTrack->setDescription(std::move(desc));

	std::lock_guard<std::mutex> lock(session->mediaMutex);
	session->payloadType = payloadType;
	session->track = offeredTrack;
}

using TrackHandler =
    std::function<void(const std::shared_ptr<Session> &, std::shared_ptr<rtc::Track>)>;

void handleOffer(Slot &slot, const TrackHandler &onTrack, const httplib::Request &req,
                 httplib::Response &res) {
	setCorsHeaders(res, "POST, OPTIONS");

	auto session = std::make_shared<Session>();
	session->id = randomResourceId();
	session->gatheringDoneFuture = session->gatheringDone.get_future().share();

	// A session that already closed or failed is reclaimed here rather than from
	// the state callback, because destroying a PeerConnection from inside its
	// own callback is asking for a deadlock.
	std::shared_ptr<Session> expired;
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		if (slot.active && !isFinished(slot.active->pc->state())) {
			res.status = 409; // Conflict
			res.set_content(std::string("A ") + slot.name +
			                    " session is already active, delete it first\n",
			                "text/plain");
			return;
		}
		expired = std::move(slot.active);
		slot.active = session;
	}
	if (expired) {
		expired->pc->close();
	}

	session->pc = std::make_shared<rtc::PeerConnection>();
	session->rtcpSession = std::make_shared<rtc::RtcpReceivingSession>();

	const std::string name = slot.name;
	session->pc->onStateChange([name](rtc::PeerConnection::State state) {
		std::cout << name << " state: " << state << std::endl;
	});

	std::weak_ptr<Session> weak = session;

	session->pc->onGatheringStateChange([weak, name](rtc::PeerConnection::GatheringState state) {
		std::cout << name << " gathering state: " << state << std::endl;
		if (state != rtc::PeerConnection::GatheringState::Complete) {
			return;
		}
		if (auto s = weak.lock()) {
			std::call_once(s->gatheringFlag, [&s]() { s->gatheringDone.set_value(); });
		}
	});

	session->pc->onTrack([weak, onTrack](std::shared_ptr<rtc::Track> offeredTrack) {
		if (auto s = weak.lock()) {
			onTrack(s, std::move(offeredTrack));
		}
	});

	try {
		session->pc->setRemoteDescription(rtc::Description(req.body, "offer"));
	} catch (const std::exception &e) {
		dropSession(slot, session);
		res.status = 400; // Bad Request
		res.set_content(std::string("Invalid SDP offer: ") + e.what() + "\n", "text/plain");
		return;
	}

	// Wait for gathering to finish. The bound matters: without it a peer that
	// never finishes gathering pins this HTTP worker forever.
	if (session->gatheringDoneFuture.wait_for(gatheringTimeout) != std::future_status::ready) {
		dropSession(slot, session);
		res.status = 504; // Gateway Timeout
		res.set_content("ICE gathering did not complete in time\n", "text/plain");
		return;
	}

	std::optional<rtc::Description> description = session->pc->localDescription();
	if (!description) {
		dropSession(slot, session);
		res.status = 500;
		res.set_content("No local description available\n", "text/plain");
		return;
	}

	// RFC 9725: the session is a resource created by the POST, so it answers 201
	// and names the resource in Location. The client needs that URL to delete
	// the session when it stops.
	res.status = 201; // Created
	res.set_header("Location", "/" + std::string(slot.name) + "/" + session->id);
	res.set_content(std::string(*description), "application/sdp");
}

void handleDelete(Slot &slot, const httplib::Request &req, httplib::Response &res) {
	setCorsHeaders(res, "DELETE, OPTIONS");

	const std::string id = req.matches[1];

	std::shared_ptr<Session> dead;
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		if (!slot.active || slot.active->id != id) {
			res.status = 404; // Not Found
			res.set_content("No such resource\n", "text/plain");
			return;
		}
		dead = std::move(slot.active);
	}

	dead->pc->close();
	std::cout << "Deleted " << slot.name << " resource " << id << std::endl;
	res.status = 200; // OK
}

// index.html sits next to the executable, copied there by CMake.
std::string executableDir(const char *argv0) {
	const std::string path = argv0 ? argv0 : "";
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

} // namespace

int main(int argc, char *argv[]) {
	// Optional arguments, so the example can be moved out of the way of whatever
	// is already using port 8080, and the page can be served from elsewhere.
	const int httpPort = argc > 1 ? std::atoi(argv[1]) : defaultHttpPort;
	if (httpPort <= 0 || httpPort > 65535) {
		std::cerr << "Usage: " << argv[0] << " [port] [web-root]" << std::endl;
		return 1;
	}
	const std::string webRoot = argc > 2 ? argv[2] : executableDir(argv[0]);

	rtc::InitLogger(rtc::LogLevel::Info);

	egressSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (egressSocket < 0) {
		std::cerr << "Failed to create UDP egress socket" << std::endl;
		return 1;
	}
	egressAddr.sin_family = AF_INET;
	egressAddr.sin_addr.s_addr = inet_addr(localHost);
	egressAddr.sin_port = htons(udpEgressPort);

	std::thread udpThread(receiveRtpFromUdp);
	// Detached because it blocks in recv() until the process exits; joining it
	// would hang shutdown, and letting a joinable thread go out of scope would
	// call std::terminate.
	udpThread.detach();

	httplib::Server svr;

	if (!svr.set_mount_point("/", webRoot)) {
		std::cerr << "Warning: no web root at " << webRoot << ", the page will not be served"
		          << std::endl;
	}

	svr.Options("/whip", [](const httplib::Request &, httplib::Response &res) {
		setCorsHeaders(res, "POST, OPTIONS");
		res.status = 204; // No Content
	});
	svr.Post("/whip", [](const httplib::Request &req, httplib::Response &res) {
		handleOffer(ingestSlot, onIngestTrack, req, res);
	});

	svr.Options("/whep", [](const httplib::Request &, httplib::Response &res) {
		setCorsHeaders(res, "POST, OPTIONS");
		res.status = 204; // No Content
	});
	svr.Post("/whep", [](const httplib::Request &req, httplib::Response &res) {
		handleOffer(egressSlot, onEgressTrack, req, res);
	});

	svr.Options("/whip/(.*)", [](const httplib::Request &, httplib::Response &res) {
		setCorsHeaders(res, "DELETE, OPTIONS");
		res.status = 204; // No Content
	});
	svr.Delete("/whip/(.*)", [](const httplib::Request &req, httplib::Response &res) {
		handleDelete(ingestSlot, req, res);
	});

	svr.Options("/whep/(.*)", [](const httplib::Request &, httplib::Response &res) {
		setCorsHeaders(res, "DELETE, OPTIONS");
		res.status = 204; // No Content
	});
	svr.Delete("/whep/(.*)", [](const httplib::Request &req, httplib::Response &res) {
		handleDelete(egressSlot, req, res);
	});

	std::cout << "Open http://localhost:" << httpPort << " in your browser" << std::endl;
	std::cout << "RTP in: udp/" << udpIngressPort << "   RTP out: udp/" << udpEgressPort
	          << std::endl;

	if (!svr.listen("0.0.0.0", httpPort)) {
		std::cerr << "Failed to listen on port " << httpPort << std::endl;
		return 1;
	}

	return 0;
}
