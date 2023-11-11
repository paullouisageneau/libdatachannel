/**
 * Copyright (c) 2019-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc.h"
#include "rtc.hpp"

#include "impl/internals.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

using namespace rtc;
using namespace std::chrono_literals;
using std::chrono::milliseconds;

namespace {

std::unordered_map<int, shared_ptr<PeerConnection>> peerConnectionMap;
std::unordered_map<int, shared_ptr<DataChannel>> dataChannelMap;
std::unordered_map<int, shared_ptr<Track>> trackMap;
#if RTC_ENABLE_MEDIA
std::unordered_map<int, shared_ptr<RtcpSrReporter>> rtcpSrReporterMap;
std::unordered_map<int, shared_ptr<RtpPacketizationConfig>> rtpConfigMap;
#endif
#if RTC_ENABLE_WEBSOCKET
std::unordered_map<int, shared_ptr<WebSocket>> webSocketMap;
std::unordered_map<int, shared_ptr<WebSocketServer>> webSocketServerMap;
#endif
std::unordered_map<int, void *> userPointerMap;
std::mutex mutex;
int lastId = 0;

optional<void *> getUserPointer(int id) {
	std::lock_guard lock(mutex);
	auto it = userPointerMap.find(id);
	return it != userPointerMap.end() ? std::make_optional(it->second) : nullopt;
}

void setUserPointer(int i, void *ptr) {
	std::lock_guard lock(mutex);
	userPointerMap[i] = ptr;
}

shared_ptr<PeerConnection> getPeerConnection(int id) {
	std::lock_guard lock(mutex);
	if (auto it = peerConnectionMap.find(id); it != peerConnectionMap.end())
		return it->second;
	else
		throw std::invalid_argument("PeerConnection ID does not exist");
}

shared_ptr<DataChannel> getDataChannel(int id) {
	std::lock_guard lock(mutex);
	if (auto it = dataChannelMap.find(id); it != dataChannelMap.end())
		return it->second;
	else
		throw std::invalid_argument("DataChannel ID does not exist");
}

shared_ptr<Track> getTrack(int id) {
	std::lock_guard lock(mutex);
	if (auto it = trackMap.find(id); it != trackMap.end())
		return it->second;
	else
		throw std::invalid_argument("Track ID does not exist");
}

int emplacePeerConnection(shared_ptr<PeerConnection> ptr) {
	std::lock_guard lock(mutex);
	int pc = ++lastId;
	peerConnectionMap.emplace(std::make_pair(pc, ptr));
	userPointerMap.emplace(std::make_pair(pc, nullptr));
	return pc;
}

int emplaceDataChannel(shared_ptr<DataChannel> ptr) {
	std::lock_guard lock(mutex);
	int dc = ++lastId;
	dataChannelMap.emplace(std::make_pair(dc, ptr));
	userPointerMap.emplace(std::make_pair(dc, nullptr));
	return dc;
}

int emplaceTrack(shared_ptr<Track> ptr) {
	std::lock_guard lock(mutex);
	int tr = ++lastId;
	trackMap.emplace(std::make_pair(tr, ptr));
	userPointerMap.emplace(std::make_pair(tr, nullptr));
	return tr;
}

void erasePeerConnection(int pc) {
	std::lock_guard lock(mutex);
	if (peerConnectionMap.erase(pc) == 0)
		throw std::invalid_argument("Peer Connection ID does not exist");
	userPointerMap.erase(pc);
}

void eraseDataChannel(int dc) {
	std::lock_guard lock(mutex);
	if (dataChannelMap.erase(dc) == 0)
		throw std::invalid_argument("Data Channel ID does not exist");
	userPointerMap.erase(dc);
}

void eraseTrack(int tr) {
	std::lock_guard lock(mutex);
	if (trackMap.erase(tr) == 0)
		throw std::invalid_argument("Track ID does not exist");
#if RTC_ENABLE_MEDIA
	rtcpSrReporterMap.erase(tr);
	rtpConfigMap.erase(tr);
#endif
	userPointerMap.erase(tr);
}

size_t eraseAll() {
	std::lock_guard lock(mutex);
	size_t count = dataChannelMap.size() + trackMap.size() + peerConnectionMap.size();
	dataChannelMap.clear();
	trackMap.clear();
	peerConnectionMap.clear();
#if RTC_ENABLE_MEDIA
	count += rtcpSrReporterMap.size() + rtpConfigMap.size();
	rtcpSrReporterMap.clear();
	rtpConfigMap.clear();
#endif
#if RTC_ENABLE_WEBSOCKET
	count += webSocketMap.size() + webSocketServerMap.size();
	webSocketMap.clear();
	webSocketServerMap.clear();
#endif
	userPointerMap.clear();
	return count;
}

shared_ptr<Channel> getChannel(int id) {
	std::lock_guard lock(mutex);
	if (auto it = dataChannelMap.find(id); it != dataChannelMap.end())
		return it->second;
	if (auto it = trackMap.find(id); it != trackMap.end())
		return it->second;
#if RTC_ENABLE_WEBSOCKET
	if (auto it = webSocketMap.find(id); it != webSocketMap.end())
		return it->second;
#endif
	throw std::invalid_argument("DataChannel, Track, or WebSocket ID does not exist");
}

void eraseChannel(int id) {
	std::lock_guard lock(mutex);
	if (dataChannelMap.erase(id) != 0) {
		userPointerMap.erase(id);
		return;
	}
	if (trackMap.erase(id) != 0) {
		userPointerMap.erase(id);
#if RTC_ENABLE_MEDIA
		rtcpSrReporterMap.erase(id);
		rtpConfigMap.erase(id);
#endif
		return;
	}
#if RTC_ENABLE_WEBSOCKET
	if (webSocketMap.erase(id) != 0) {
		userPointerMap.erase(id);
		return;
	}
#endif
	throw std::invalid_argument("DataChannel, Track, or WebSocket ID does not exist");
}

int copyAndReturn(string s, char *buffer, int size) {
	if (!buffer)
		return int(s.size() + 1);

	if (size < int(s.size() + 1))
		return RTC_ERR_TOO_SMALL;

	std::copy(s.begin(), s.end(), buffer);
	buffer[s.size()] = '\0';
	return int(s.size() + 1);
}

int copyAndReturn(binary b, char *buffer, int size) {
	if (!buffer)
		return int(b.size());

	if (size < int(b.size()))
		return RTC_ERR_TOO_SMALL;

	auto data = reinterpret_cast<const char *>(b.data());
	std::copy(data, data + b.size(), buffer);
	return int(b.size());
}

template <typename T> int copyAndReturn(std::vector<T> b, T *buffer, int size) {
	if (!buffer)
		return int(b.size());

	if (size < int(b.size()))
		return RTC_ERR_TOO_SMALL;
	std::copy(b.begin(), b.end(), buffer);
	return int(b.size());
}

template <typename F> int wrap(F func) {
	try {
		return int(func());

	} catch (const std::invalid_argument &e) {
		PLOG_ERROR << e.what();
		return RTC_ERR_INVALID;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		return RTC_ERR_FAILURE;
	}
}

#if RTC_ENABLE_MEDIA

string lowercased(string str) {
	std::transform(str.begin(), str.end(), str.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return str;
}

shared_ptr<RtcpSrReporter> getRtcpSrReporter(int id) {
	std::lock_guard lock(mutex);
	if (auto it = rtcpSrReporterMap.find(id); it != rtcpSrReporterMap.end()) {
		return it->second;
	} else {
		throw std::invalid_argument("RTCP SR reporter ID does not exist");
	}
}

void emplaceRtcpSrReporter(shared_ptr<RtcpSrReporter> ptr, int tr) {
	std::lock_guard lock(mutex);
	rtcpSrReporterMap.emplace(std::make_pair(tr, ptr));
}

shared_ptr<RtpPacketizationConfig> getRtpConfig(int id) {
	std::lock_guard lock(mutex);
	if (auto it = rtpConfigMap.find(id); it != rtpConfigMap.end()) {
		return it->second;
	} else {
		throw std::invalid_argument("RTP configuration ID does not exist");
	}
}

void emplaceRtpConfig(shared_ptr<RtpPacketizationConfig> ptr, int tr) {
	std::lock_guard lock(mutex);
	rtpConfigMap.emplace(std::make_pair(tr, ptr));
}

shared_ptr<RtpPacketizationConfig>
createRtpPacketizationConfig(const rtcPacketizationHandlerInit *init) {
	if (!init)
		throw std::invalid_argument("Unexpected null pointer for packetization handler init");

	if (!init->cname)
		throw std::invalid_argument("Unexpected null pointer for cname");

	auto config = std::make_shared<RtpPacketizationConfig>(init->ssrc, init->cname,
	                                                       init->payloadType, init->clockRate);
	config->sequenceNumber = init->sequenceNumber;
	config->timestamp = init->timestamp;
	return config;
}

class MediaInterceptor final : public MediaHandler {
public:
	using MessageCallback = std::function<void *(void *data, int size)>;

	MediaInterceptor(MessageCallback cb) : incomingCallback(cb) {}

	// Called when there is traffic coming from the peer
	void incoming(message_vector &messages,
	              [[maybe_unused]] const message_callback &send) override {
		// If no callback is provided, just forward the message on
		if (!incomingCallback)
			return;

		message_vector result;
		for (auto &msg : messages) {
			auto res = incomingCallback(reinterpret_cast<void *>(msg->data()), int(msg->size()));

			// If a null pointer was returned, drop the incoming message
			if (!res)
				continue;

			if (res == msg->data()) {
				// If the original data pointer was returned, forward the incoming message
				result.push_back(std::move(msg));
			} else {
				// else construct a true message_ptr from the returned opaque pointer
				result.push_back(
				    make_message_from_opaque_ptr(std::move(reinterpret_cast<rtcMessage *>(res))));
			}
		}
	}

private:
	MessageCallback incomingCallback;
};

#endif // RTC_ENABLE_MEDIA

#if RTC_ENABLE_WEBSOCKET

shared_ptr<WebSocket> getWebSocket(int id) {
	std::lock_guard lock(mutex);
	if (auto it = webSocketMap.find(id); it != webSocketMap.end())
		return it->second;
	else
		throw std::invalid_argument("WebSocket ID does not exist");
}

int emplaceWebSocket(shared_ptr<WebSocket> ptr) {
	std::lock_guard lock(mutex);
	int ws = ++lastId;
	webSocketMap.emplace(std::make_pair(ws, ptr));
	userPointerMap.emplace(std::make_pair(ws, nullptr));
	return ws;
}

void eraseWebSocket(int ws) {
	std::lock_guard lock(mutex);
	if (webSocketMap.erase(ws) == 0)
		throw std::invalid_argument("WebSocket ID does not exist");
	userPointerMap.erase(ws);
}

shared_ptr<WebSocketServer> getWebSocketServer(int id) {
	std::lock_guard lock(mutex);
	if (auto it = webSocketServerMap.find(id); it != webSocketServerMap.end())
		return it->second;
	else
		throw std::invalid_argument("WebSocketServer ID does not exist");
}

int emplaceWebSocketServer(shared_ptr<WebSocketServer> ptr) {
	std::lock_guard lock(mutex);
	int wsserver = ++lastId;
	webSocketServerMap.emplace(std::make_pair(wsserver, ptr));
	userPointerMap.emplace(std::make_pair(wsserver, nullptr));
	return wsserver;
}

void eraseWebSocketServer(int wsserver) {
	std::lock_guard lock(mutex);
	if (webSocketServerMap.erase(wsserver) == 0)
		throw std::invalid_argument("WebSocketServer ID does not exist");
	userPointerMap.erase(wsserver);
}

#endif

} // namespace

void rtcInitLogger(rtcLogLevel level, rtcLogCallbackFunc cb) {
	LogCallback callback = nullptr;
	if (cb)
		callback = [cb](LogLevel level, string message) {
			cb(static_cast<rtcLogLevel>(level), message.c_str());
		};

	InitLogger(static_cast<LogLevel>(level), callback);
}

void rtcSetUserPointer(int i, void *ptr) { setUserPointer(i, ptr); }

void *rtcGetUserPointer(int i) { return getUserPointer(i).value_or(nullptr); }

int rtcCreatePeerConnection(const rtcConfiguration *config) {
	return wrap([config] {
		Configuration c;
		for (int i = 0; i < config->iceServersCount; ++i)
			c.iceServers.emplace_back(string(config->iceServers[i]));

		if (config->proxyServer)
			c.proxyServer.emplace(config->proxyServer);

		if (config->bindAddress)
			c.bindAddress = string(config->bindAddress);

		if (config->portRangeBegin > 0 || config->portRangeEnd > 0) {
			c.portRangeBegin = config->portRangeBegin;
			c.portRangeEnd = config->portRangeEnd;
		}

		c.certificateType = static_cast<CertificateType>(config->certificateType);
		c.iceTransportPolicy = static_cast<TransportPolicy>(config->iceTransportPolicy);
		c.enableIceTcp = config->enableIceTcp;
		c.enableIceUdpMux = config->enableIceUdpMux;
		c.disableAutoNegotiation = config->disableAutoNegotiation;
		c.forceMediaTransport = config->forceMediaTransport;

		if (config->mtu > 0)
			c.mtu = size_t(config->mtu);

		if (config->maxMessageSize)
			c.maxMessageSize = size_t(config->maxMessageSize);

		return emplacePeerConnection(std::make_shared<PeerConnection>(std::move(c)));
	});
}

int rtcClosePeerConnection(int pc) {
	return wrap([pc] {
		auto peerConnection = getPeerConnection(pc);
		peerConnection->close();
		return RTC_ERR_SUCCESS;
	});
}

int rtcDeletePeerConnection(int pc) {
	return wrap([pc] {
		auto peerConnection = getPeerConnection(pc);
		peerConnection->close();
		erasePeerConnection(pc);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetLocalDescriptionCallback(int pc, rtcDescriptionCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onLocalDescription([pc, cb](Description desc) {
				if (auto ptr = getUserPointer(pc))
					cb(pc, string(desc).c_str(), desc.typeString().c_str(), *ptr);
			});
		else
			peerConnection->onLocalDescription(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetLocalCandidateCallback(int pc, rtcCandidateCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onLocalCandidate([pc, cb](Candidate cand) {
				if (auto ptr = getUserPointer(pc))
					cb(pc, cand.candidate().c_str(), cand.mid().c_str(), *ptr);
			});
		else
			peerConnection->onLocalCandidate(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetStateChangeCallback(int pc, rtcStateChangeCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onStateChange([pc, cb](PeerConnection::State state) {
				if (auto ptr = getUserPointer(pc))
					cb(pc, static_cast<rtcState>(state), *ptr);
			});
		else
			peerConnection->onStateChange(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetIceStateChangeCallback(int pc, rtcIceStateChangeCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onIceStateChange([pc, cb](PeerConnection::IceState state) {
				if (auto ptr = getUserPointer(pc))
					cb(pc, static_cast<rtcIceState>(state), *ptr);
			});
		else
			peerConnection->onIceStateChange(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetGatheringStateChangeCallback(int pc, rtcGatheringStateCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onGatheringStateChange([pc, cb](PeerConnection::GatheringState state) {
				if (auto ptr = getUserPointer(pc))
					cb(pc, static_cast<rtcGatheringState>(state), *ptr);
			});
		else
			peerConnection->onGatheringStateChange(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetSignalingStateChangeCallback(int pc, rtcSignalingStateCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onSignalingStateChange([pc, cb](PeerConnection::SignalingState state) {
				if (auto ptr = getUserPointer(pc))
					cb(pc, static_cast<rtcSignalingState>(state), *ptr);
			});
		else
			peerConnection->onGatheringStateChange(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetDataChannelCallback(int pc, rtcDataChannelCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onDataChannel([pc, cb](shared_ptr<DataChannel> dataChannel) {
				int dc = emplaceDataChannel(dataChannel);
				if (auto ptr = getUserPointer(pc)) {
					rtcSetUserPointer(dc, *ptr);
					cb(pc, dc, *ptr);
				}
			});
		else
			peerConnection->onDataChannel(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetTrackCallback(int pc, rtcTrackCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onTrack([pc, cb](shared_ptr<Track> track) {
				int tr = emplaceTrack(track);
				if (auto ptr = getUserPointer(pc)) {
					rtcSetUserPointer(tr, *ptr);
					cb(pc, tr, *ptr);
				}
			});
		else
			peerConnection->onTrack(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetLocalDescription(int pc, const char *type) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		peerConnection->setLocalDescription(type ? Description::stringToType(type)
		                                         : Description::Type::Unspec);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (!sdp)
			throw std::invalid_argument("Unexpected null pointer for remote description");

		peerConnection->setRemoteDescription({string(sdp), type ? string(type) : ""});
		return RTC_ERR_SUCCESS;
	});
}

int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (!cand)
			throw std::invalid_argument("Unexpected null pointer for remote candidate");

		peerConnection->addRemoteCandidate({string(cand), mid ? string(mid) : ""});
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetLocalDescription(int pc, char *buffer, int size) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (auto desc = peerConnection->localDescription())
			return copyAndReturn(string(*desc), buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetRemoteDescription(int pc, char *buffer, int size) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (auto desc = peerConnection->remoteDescription())
			return copyAndReturn(string(*desc), buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetLocalDescriptionType(int pc, char *buffer, int size) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (auto desc = peerConnection->localDescription())
			return copyAndReturn(desc->typeString(), buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetRemoteDescriptionType(int pc, char *buffer, int size) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (auto desc = peerConnection->remoteDescription())
			return copyAndReturn(desc->typeString(), buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetLocalAddress(int pc, char *buffer, int size) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (auto addr = peerConnection->localAddress())
			return copyAndReturn(std::move(*addr), buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetRemoteAddress(int pc, char *buffer, int size) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (auto addr = peerConnection->remoteAddress())
			return copyAndReturn(std::move(*addr), buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetSelectedCandidatePair(int pc, char *local, int localSize, char *remote, int remoteSize) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		Candidate localCand;
		Candidate remoteCand;
		if (!peerConnection->getSelectedCandidatePair(&localCand, &remoteCand))
			return RTC_ERR_NOT_AVAIL;

		int localRet = copyAndReturn(string(localCand), local, localSize);
		if (localRet < 0)
			return localRet;

		int remoteRet = copyAndReturn(string(remoteCand), remote, remoteSize);
		if (remoteRet < 0)
			return remoteRet;

		return std::max(localRet, remoteRet);
	});
}

int rtcGetMaxDataChannelStream(int pc) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		return int(peerConnection->maxDataChannelId());
	});
}

int rtcGetRemoteMaxMessageSize(int pc) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);
		return int(peerConnection->remoteMaxMessageSize());
	});
}

int rtcSetOpenCallback(int id, rtcOpenCallbackFunc cb) {
	return wrap([&] {
		auto channel = getChannel(id);
		if (cb)
			channel->onOpen([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(id, *ptr);
			});
		else
			channel->onOpen(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetClosedCallback(int id, rtcClosedCallbackFunc cb) {
	return wrap([&] {
		auto channel = getChannel(id);
		if (cb)
			channel->onClosed([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(id, *ptr);
			});
		else
			channel->onClosed(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetErrorCallback(int id, rtcErrorCallbackFunc cb) {
	return wrap([&] {
		auto channel = getChannel(id);
		if (cb)
			channel->onError([id, cb](string error) {
				if (auto ptr = getUserPointer(id))
					cb(id, error.c_str(), *ptr);
			});
		else
			channel->onError(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetMessageCallback(int id, rtcMessageCallbackFunc cb) {
	return wrap([&] {
		auto channel = getChannel(id);
		if (cb)
			channel->onMessage(
			    [id, cb](binary b) {
				    if (auto ptr = getUserPointer(id))
					    cb(id, reinterpret_cast<const char *>(b.data()), int(b.size()), *ptr);
			    },
			    [id, cb](string s) {
				    if (auto ptr = getUserPointer(id))
					    cb(id, s.c_str(), -int(s.size() + 1), *ptr);
			    });
		else
			channel->onMessage(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSendMessage(int id, const char *data, int size) {
	return wrap([&] {
		auto channel = getChannel(id);

		if (!data && size != 0)
			throw std::invalid_argument("Unexpected null pointer for data");

		if (size >= 0) {
			auto b = reinterpret_cast<const byte *>(data);
			channel->send(binary(b, b + size));
		} else {
			channel->send(string(data));
		}
		return RTC_ERR_SUCCESS;
	});
}

int rtcClose(int id) {
	return wrap([&] {
		auto channel = getChannel(id);
		channel->close();
		return RTC_ERR_SUCCESS;
	});
}

int rtcDelete(int id) {
	return wrap([id] {
		auto channel = getChannel(id);
		channel->close();
		eraseChannel(id);
		return RTC_ERR_SUCCESS;
	});
}

bool rtcIsOpen(int id) {
	return wrap([id] { return getChannel(id)->isOpen() ? 0 : 1; }) == 0 ? true : false;
}

bool rtcIsClosed(int id) {
	return wrap([id] { return getChannel(id)->isClosed() ? 0 : 1; }) == 0 ? true : false;
}

int rtcMaxMessageSize(int id) {
	return wrap([id] {
		auto channel = getChannel(id);
		return int(channel->maxMessageSize());
	});
}

int rtcGetBufferedAmount(int id) {
	return wrap([id] {
		auto channel = getChannel(id);
		return int(channel->bufferedAmount());
	});
}

int rtcSetBufferedAmountLowThreshold(int id, int amount) {
	return wrap([&] {
		auto channel = getChannel(id);
		channel->setBufferedAmountLowThreshold(size_t(amount));
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetBufferedAmountLowCallback(int id, rtcBufferedAmountLowCallbackFunc cb) {
	return wrap([&] {
		auto channel = getChannel(id);
		if (cb)
			channel->onBufferedAmountLow([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(id, *ptr);
			});
		else
			channel->onBufferedAmountLow(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetAvailableAmount(int id) {
	return wrap([id] { return int(getChannel(id)->availableAmount()); });
}

int rtcSetAvailableCallback(int id, rtcAvailableCallbackFunc cb) {
	return wrap([&] {
		auto channel = getChannel(id);
		if (cb)
			channel->onAvailable([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(id, *ptr);
			});
		else
			channel->onAvailable(nullptr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcReceiveMessage(int id, char *buffer, int *size) {
	return wrap([&] {
		auto channel = getChannel(id);

		if (!size)
			throw std::invalid_argument("Unexpected null pointer for size");

		*size = std::abs(*size);

		auto message = channel->peek();
		if (!message)
			return RTC_ERR_NOT_AVAIL;

		return std::visit( //
		    overloaded{
		        [&](binary b) {
			        int ret = copyAndReturn(std::move(b), buffer, *size);
			        if (ret >= 0) {
				        *size = ret;
				        if (buffer) {
					        channel->receive(); // discard
				        }

				        return RTC_ERR_SUCCESS;
			        } else {
				        *size = int(b.size());
				        return ret;
			        }
		        },
		        [&](string s) {
			        int ret = copyAndReturn(std::move(s), buffer, *size);
			        if (ret >= 0) {
				        *size = -ret;
				        if (buffer) {
					        channel->receive(); // discard
				        }

				        return RTC_ERR_SUCCESS;
			        } else {
				        *size = -int(s.size() + 1);
				        return ret;
			        }
		        },
		    },
		    *message);
	});
}

int rtcCreateDataChannel(int pc, const char *label) {
	return rtcCreateDataChannelEx(pc, label, nullptr);
}

int rtcCreateDataChannelEx(int pc, const char *label, const rtcDataChannelInit *init) {
	return wrap([&] {
		DataChannelInit dci = {};
		if (init) {
			auto *reliability = &init->reliability;
			dci.reliability.unordered = reliability->unordered;
			if (reliability->unreliable) {
				if (reliability->maxPacketLifeTime > 0)
					dci.reliability.maxPacketLifeTime.emplace(milliseconds(reliability->maxPacketLifeTime));
				else
					dci.reliability.maxRetransmits.emplace(reliability->maxRetransmits);
			}

			dci.negotiated = init->negotiated;
			dci.id = init->manualStream ? std::make_optional(init->stream) : nullopt;
			dci.protocol = init->protocol ? init->protocol : "";
		}

		auto peerConnection = getPeerConnection(pc);
		int dc = emplaceDataChannel(
		    peerConnection->createDataChannel(string(label ? label : ""), std::move(dci)));

		if (auto ptr = getUserPointer(pc))
			rtcSetUserPointer(dc, *ptr);

		return dc;
	});
}

int rtcDeleteDataChannel(int dc) {
	return wrap([dc] {
		auto dataChannel = getDataChannel(dc);
		dataChannel->close();
		eraseDataChannel(dc);
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetDataChannelStream(int dc) {
	return wrap([dc] {
		auto dataChannel = getDataChannel(dc);
		if (auto stream = dataChannel->stream())
			return int(*stream);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetDataChannelLabel(int dc, char *buffer, int size) {
	return wrap([&] {
		auto dataChannel = getDataChannel(dc);
		return copyAndReturn(dataChannel->label(), buffer, size);
	});
}

int rtcGetDataChannelProtocol(int dc, char *buffer, int size) {
	return wrap([&] {
		auto dataChannel = getDataChannel(dc);
		return copyAndReturn(dataChannel->protocol(), buffer, size);
	});
}

int rtcGetDataChannelReliability(int dc, rtcReliability *reliability) {
	return wrap([&] {
		auto dataChannel = getDataChannel(dc);

		if (!reliability)
			throw std::invalid_argument("Unexpected null pointer for reliability");

		Reliability dcr = dataChannel->reliability();
		std::memset(reliability, 0, sizeof(*reliability));
		reliability->unordered = dcr.unordered;
		if(dcr.maxPacketLifeTime) {
			reliability->unreliable = true;
			reliability->maxPacketLifeTime = static_cast<unsigned int>(dcr.maxPacketLifeTime->count());
		} else if (dcr.maxRetransmits) {
			reliability->unreliable = true;
			reliability->maxRetransmits = *dcr.maxRetransmits;
		} else {
			reliability->unreliable = false;
		}
		return RTC_ERR_SUCCESS;
	});
}

int rtcAddTrack(int pc, const char *mediaDescriptionSdp) {
	return wrap([&] {
		if (!mediaDescriptionSdp)
			throw std::invalid_argument("Unexpected null pointer for track media description");

		auto peerConnection = getPeerConnection(pc);
		Description::Media media{string(mediaDescriptionSdp)};
		int tr = emplaceTrack(peerConnection->addTrack(std::move(media)));
		if (auto ptr = getUserPointer(pc))
			rtcSetUserPointer(tr, *ptr);

		return tr;
	});
}

int rtcAddTrackEx(int pc, const rtcTrackInit *init) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (!init)
			throw std::invalid_argument("Unexpected null pointer for track init");

		auto direction = static_cast<Description::Direction>(init->direction);

		string mid;
		if (init->mid) {
			mid = string(init->mid);
		} else {
			switch (init->codec) {
			case RTC_CODEC_AV1:
			case RTC_CODEC_H264:
			case RTC_CODEC_H265:
			case RTC_CODEC_VP8:
			case RTC_CODEC_VP9:
				mid = "video";
				break;
			case RTC_CODEC_OPUS:
			case RTC_CODEC_PCMU:
			case RTC_CODEC_PCMA:
			case RTC_CODEC_AAC:
				mid = "audio";
				break;
			default:
				mid = "video";
				break;
			}
		}

		int pt = init->payloadType;
		auto profile = init->profile ? std::make_optional(string(init->profile)) : nullopt;

		unique_ptr<Description::Media> description;
		switch (init->codec) {
		case RTC_CODEC_AV1:
		case RTC_CODEC_H264:
		case RTC_CODEC_H265:
		case RTC_CODEC_VP8:
		case RTC_CODEC_VP9: {
			auto video = std::make_unique<Description::Video>(mid, direction);
			switch (init->codec) {
			case RTC_CODEC_AV1:
				video->addAV1Codec(pt, profile);
				break;
			case RTC_CODEC_H264:
				video->addH264Codec(pt, profile);
				break;
			case RTC_CODEC_H265:
				video->addH265Codec(pt, profile);
				break;
			case RTC_CODEC_VP8:
				video->addVP8Codec(pt, profile);
				break;
			case RTC_CODEC_VP9:
				video->addVP9Codec(pt, profile);
				break;
			default:
				break;
			}
			description = std::move(video);
			break;
		}
		case RTC_CODEC_OPUS:
		case RTC_CODEC_PCMU:
		case RTC_CODEC_PCMA:
		case RTC_CODEC_AAC: {
			auto audio = std::make_unique<Description::Audio>(mid, direction);
			switch (init->codec) {
			case RTC_CODEC_OPUS:
				audio->addOpusCodec(pt, profile);
				break;
			case RTC_CODEC_PCMU:
				audio->addPCMUCodec(pt, profile);
				break;
			case RTC_CODEC_PCMA:
				audio->addPCMACodec(pt, profile);
				break;
			case RTC_CODEC_AAC:
				audio->addAACCodec(pt, profile);
				break;
			default:
				break;
			}
			description = std::move(audio);
			break;
		}
		default:
			break;
		}

		if (!description)
			throw std::invalid_argument("Unexpected codec");

		description->addSSRC(init->ssrc,
		                     init->name ? std::make_optional(string(init->name)) : nullopt,
		                     init->msid ? std::make_optional(string(init->msid)) : nullopt,
		                     init->trackId ? std::make_optional(string(init->trackId)) : nullopt);

		int tr = emplaceTrack(peerConnection->addTrack(std::move(*description)));

		if (auto ptr = getUserPointer(pc))
			rtcSetUserPointer(tr, *ptr);

		return tr;
	});
}

int rtcDeleteTrack(int tr) {
	return wrap([&] {
		auto track = getTrack(tr);
		track->close();
		eraseTrack(tr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetTrackDescription(int tr, char *buffer, int size) {
	return wrap([&] {
		auto track = getTrack(tr);
		return copyAndReturn(track->description(), buffer, size);
	});
}

int rtcGetTrackMid(int tr, char *buffer, int size) {
	return wrap([&] {
		auto track = getTrack(tr);
		return copyAndReturn(track->mid(), buffer, size);
	});
}

int rtcGetTrackDirection(int tr, rtcDirection *direction) {
	return wrap([&] {
		if (!direction)
			throw std::invalid_argument("Unexpected null pointer for track direction");

		auto track = getTrack(tr);
		*direction = static_cast<rtcDirection>(track->direction());
		return RTC_ERR_SUCCESS;
	});
}

#if RTC_ENABLE_MEDIA

void setSSRC(Description::Media *description, uint32_t ssrc, const char *_name, const char *_msid,
             const char *_trackID) {

	optional<string> name = nullopt;
	if (_name) {
		name = string(_name);
	}

	optional<string> msid = nullopt;
	if (_msid) {
		msid = string(_msid);
	}

	optional<string> trackID = nullopt;
	if (_trackID) {
		trackID = string(_trackID);
	}

	description->addSSRC(ssrc, name, msid, trackID);
}

rtcMessage *rtcCreateOpaqueMessage(void *data, int size) {
	auto src = reinterpret_cast<std::byte *>(data);
	auto msg = new Message(src, src + size);
	// Downgrade the message pointer to the opaque rtcMessage* type
	return reinterpret_cast<rtcMessage *>(msg);
}

void rtcDeleteOpaqueMessage(rtcMessage *msg) {
	// Cast the opaque pointer back to it's true type before deleting
	delete reinterpret_cast<Message *>(msg);
}

int rtcSetMediaInterceptorCallback(int pc, rtcInterceptorCallbackFunc cb) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		if (cb == nullptr) {
			peerConnection->setMediaHandler(nullptr);
			return RTC_ERR_SUCCESS;
		}

		auto interceptor = std::make_shared<MediaInterceptor>([pc, cb](void *data, int size) {
			if (auto ptr = getUserPointer(pc))
				return cb(pc, reinterpret_cast<const char *>(data), size, *ptr);
			return data;
		});

		peerConnection->setMediaHandler(interceptor);

		return RTC_ERR_SUCCESS;
	});
}

int rtcSetH264PacketizationHandler(int tr, const rtcPacketizationHandlerInit *init) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = createRtpPacketizationConfig(init);
		emplaceRtpConfig(rtpConfig, tr);
		// create packetizer
		auto nalSeparator = init ? init->nalSeparator : RTC_NAL_SEPARATOR_LENGTH;
		auto maxFragmentSize = init && init->maxFragmentSize ? init->maxFragmentSize
		                                                     : RTC_DEFAULT_MAXIMUM_FRAGMENT_SIZE;
		auto packetizer = std::make_shared<H264RtpPacketizer>(
		    static_cast<rtc::NalUnit::Separator>(nalSeparator), rtpConfig, maxFragmentSize);
		track->setMediaHandler(packetizer);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetH265PacketizationHandler(int tr, const rtcPacketizationHandlerInit *init) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = createRtpPacketizationConfig(init);
		// create packetizer
		auto nalSeparator = init ? init->nalSeparator : RTC_NAL_SEPARATOR_LENGTH;
		auto maxFragmentSize = init && init->maxFragmentSize ? init->maxFragmentSize
		                                                     : RTC_DEFAULT_MAXIMUM_FRAGMENT_SIZE;
		auto packetizer = std::make_shared<H265RtpPacketizer>(
		    static_cast<rtc::NalUnit::Separator>(nalSeparator), rtpConfig, maxFragmentSize);
		track->setMediaHandler(packetizer);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetAV1PacketizationHandler(int tr, const rtcPacketizationHandlerInit *init) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = createRtpPacketizationConfig(init);
		// create packetizer
		auto maxFragmentSize = init && init->maxFragmentSize ? init->maxFragmentSize
		                                                     : RTC_DEFAULT_MAXIMUM_FRAGMENT_SIZE;
		auto packetization = init->obuPacketization == RTC_OBU_PACKETIZED_TEMPORAL_UNIT
		                         ? AV1RtpPacketizer::Packetization::TemporalUnit
		                         : AV1RtpPacketizer::Packetization::Obu;
		auto packetizer =
		    std::make_shared<AV1RtpPacketizer>(packetization, rtpConfig, maxFragmentSize);
		track->setMediaHandler(packetizer);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetOpusPacketizationHandler(int tr, const rtcPacketizationHandlerInit *init) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = createRtpPacketizationConfig(init);
		emplaceRtpConfig(rtpConfig, tr);
		// create packetizer
		auto packetizer = std::make_shared<OpusRtpPacketizer>(rtpConfig);
		track->setMediaHandler(packetizer);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetAACPacketizationHandler(int tr, const rtcPacketizationHandlerInit *init) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = createRtpPacketizationConfig(init);
		// create packetizer
		auto packetizer = std::make_shared<AACRtpPacketizer>(rtpConfig);
		track->setMediaHandler(packetizer);
		return RTC_ERR_SUCCESS;
	});
}

int rtcChainRtcpSrReporter(int tr) {
	return wrap([&] {
		auto track = getTrack(tr);
		auto config = getRtpConfig(tr);
		auto reporter = std::make_shared<RtcpSrReporter>(config);
		track->chainMediaHandler(reporter);
		emplaceRtcpSrReporter(reporter, tr);
		return RTC_ERR_SUCCESS;
	});
}

int rtcChainRtcpNackResponder(int tr, unsigned int maxStoredPacketsCount) {
	return wrap([&] {
		auto track = getTrack(tr);
		auto responder = std::make_shared<RtcpNackResponder>(maxStoredPacketsCount);
		track->chainMediaHandler(responder);
		return RTC_ERR_SUCCESS;
	});
}

int rtcChainPliHandler(int tr, rtcPliHandlerCallbackFunc cb) {
	return wrap([&] {
		auto track = getTrack(tr);
		auto handler = std::make_shared<PliHandler>([tr, cb] {
			if (auto ptr = getUserPointer(tr))
				cb(tr, *ptr);
		});
		track->chainMediaHandler(handler);
		return RTC_ERR_SUCCESS;
	});
}

int rtcTransformSecondsToTimestamp(int id, double seconds, uint32_t *timestamp) {
	return wrap([&] {
		auto config = getRtpConfig(id);
		if (timestamp)
			*timestamp = config->secondsToTimestamp(seconds);

		return RTC_ERR_SUCCESS;
	});
}

int rtcTransformTimestampToSeconds(int id, uint32_t timestamp, double *seconds) {
	return wrap([&] {
		auto config = getRtpConfig(id);
		if (seconds)
			*seconds = config->timestampToSeconds(timestamp);

		return RTC_ERR_SUCCESS;
	});
}

int rtcGetCurrentTrackTimestamp(int id, uint32_t *timestamp) {
	return wrap([&] {
		auto config = getRtpConfig(id);
		if (timestamp)
			*timestamp = config->timestamp;

		return RTC_ERR_SUCCESS;
	});
}

int rtcSetTrackRtpTimestamp(int id, uint32_t timestamp) {
	return wrap([&] {
		auto config = getRtpConfig(id);
		config->timestamp = timestamp;
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetLastTrackSenderReportTimestamp(int id, uint32_t *timestamp) {
	return wrap([&] {
		auto sender = getRtcpSrReporter(id);
		if (timestamp)
			*timestamp = sender->lastReportedTimestamp();

		return RTC_ERR_SUCCESS;
	});
}

int rtcSetNeedsToSendRtcpSr(int id) {
	return wrap([id] {
		auto sender = getRtcpSrReporter(id);
		sender->setNeedsToReport();
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetTrackPayloadTypesForCodec(int tr, const char *ccodec, int *buffer, int size) {
	return wrap([&] {
		auto track = getTrack(tr);
		auto codec = lowercased(string(ccodec));
		auto description = track->description();
		std::vector<int> payloadTypes;
		for (int pt : description.payloadTypes())
			if (lowercased(description.rtpMap(pt)->format) == codec)
				payloadTypes.push_back(pt);

		return copyAndReturn(payloadTypes, buffer, size);
	});
}

int rtcGetSsrcsForTrack(int tr, uint32_t *buffer, int count) {
	return wrap([&] {
		auto track = getTrack(tr);
		auto ssrcs = track->description().getSSRCs();
		return copyAndReturn(ssrcs, buffer, count);
	});
}

int rtcGetCNameForSsrc(int tr, uint32_t ssrc, char *cname, int cnameSize) {
	return wrap([&] {
		auto track = getTrack(tr);
		auto description = track->description();
		auto optCName = description.getCNameForSsrc(ssrc);
		if (optCName.has_value()) {
			return copyAndReturn(optCName.value(), cname, cnameSize);
		} else {
			return 0;
		}
	});
}

int rtcGetSsrcsForType(const char *mediaType, const char *sdp, uint32_t *buffer, int bufferSize) {
	return wrap([&] {
		auto type = lowercased(string(mediaType));
		auto oldSDP = string(sdp);
		auto description = Description(oldSDP, "unspec");
		auto mediaCount = description.mediaCount();
		for (unsigned int i = 0; i < mediaCount; i++) {
			if (std::holds_alternative<Description::Media *>(description.media(i))) {
				auto media = std::get<Description::Media *>(description.media(i));
				auto currentMediaType = lowercased(media->type());
				if (currentMediaType == type) {
					auto ssrcs = media->getSSRCs();
					return copyAndReturn(ssrcs, buffer, bufferSize);
				}
			}
		}
		return 0;
	});
}

int rtcSetSsrcForType(const char *mediaType, const char *sdp, char *buffer, const int bufferSize,
                      rtcSsrcForTypeInit *init) {
	return wrap([&] {
		auto type = lowercased(string(mediaType));
		auto prevSDP = string(sdp);
		auto description = Description(prevSDP, "unspec");
		auto mediaCount = description.mediaCount();
		for (unsigned int i = 0; i < mediaCount; i++) {
			if (std::holds_alternative<Description::Media *>(description.media(i))) {
				auto media = std::get<Description::Media *>(description.media(i));
				auto currentMediaType = lowercased(media->type());
				if (currentMediaType == type) {
					setSSRC(media, init->ssrc, init->name, init->msid, init->trackId);
					break;
				}
			}
		}
		return copyAndReturn(string(description), buffer, bufferSize);
	});
}

#endif // RTC_ENABLE_MEDIA

#if RTC_ENABLE_WEBSOCKET

int rtcCreateWebSocket(const char *url) {
	return wrap([&] {
		auto webSocket = std::make_shared<WebSocket>();
		webSocket->open(url);
		return emplaceWebSocket(webSocket);
	});
}

int rtcCreateWebSocketEx(const char *url, const rtcWsConfiguration *config) {
	return wrap([&] {
		if (!url)
			throw std::invalid_argument("Unexpected null pointer for URL");

		if (!config)
			throw std::invalid_argument("Unexpected null pointer for config");

		WebSocket::Configuration c;
		c.disableTlsVerification = config->disableTlsVerification;

		if (config->proxyServer)
			c.proxyServer.emplace(config->proxyServer);

		for (int i = 0; i < config->protocolsCount; ++i)
			c.protocols.emplace_back(string(config->protocols[i]));

		if (config->connectionTimeoutMs > 0)
			c.connectionTimeout = milliseconds(config->connectionTimeoutMs);
		else if (config->connectionTimeoutMs < 0)
			c.connectionTimeout = milliseconds::zero(); // setting to 0 disables,
			                                            // not setting keeps default
		if (config->pingIntervalMs > 0)
			c.pingInterval = milliseconds(config->pingIntervalMs);
		else if (config->pingIntervalMs < 0)
			c.pingInterval = milliseconds::zero(); // setting to 0 disables,
			                                       // not setting keeps default
		if (config->maxOutstandingPings > 0)
			c.maxOutstandingPings = config->maxOutstandingPings;
		else if (config->maxOutstandingPings < 0)
			c.maxOutstandingPings = 0; // setting to 0 disables, not setting keeps default

		auto webSocket = std::make_shared<WebSocket>(std::move(c));
		webSocket->open(url);
		return emplaceWebSocket(webSocket);
	});
}

int rtcDeleteWebSocket(int ws) {
	return wrap([&] {
		auto webSocket = getWebSocket(ws);
		webSocket->forceClose();
		webSocket->resetCallbacks(); // not done on close by WebSocket
		eraseWebSocket(ws);
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetWebSocketRemoteAddress(int ws, char *buffer, int size) {
	return wrap([&] {
		auto webSocket = getWebSocket(ws);
		if (auto remoteAddress = webSocket->remoteAddress())
			return copyAndReturn(*remoteAddress, buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

int rtcGetWebSocketPath(int ws, char *buffer, int size) {
	return wrap([&] {
		auto webSocket = getWebSocket(ws);
		if (auto path = webSocket->path())
			return copyAndReturn(*path, buffer, size);
		else
			return RTC_ERR_NOT_AVAIL;
	});
}

RTC_C_EXPORT int rtcCreateWebSocketServer(const rtcWsServerConfiguration *config,
                                          rtcWebSocketClientCallbackFunc cb) {
	return wrap([&] {
		if (!config)
			throw std::invalid_argument("Unexpected null pointer for config");

		if (!cb)
			throw std::invalid_argument("Unexpected null pointer for client callback");

		WebSocketServer::Configuration c;
		c.port = config->port;
		c.enableTls = config->enableTls;
		c.certificatePemFile = config->certificatePemFile
		                           ? make_optional(string(config->certificatePemFile))
		                           : nullopt;
		c.keyPemFile = config->keyPemFile ? make_optional(string(config->keyPemFile)) : nullopt;
		c.keyPemPass = config->keyPemPass ? make_optional(string(config->keyPemPass)) : nullopt;
		c.bindAddress = config->bindAddress ? make_optional(string(config->bindAddress)) : nullopt;
		auto webSocketServer = std::make_shared<WebSocketServer>(std::move(c));
		int wsserver = emplaceWebSocketServer(webSocketServer);

		webSocketServer->onClient([wsserver, cb](shared_ptr<WebSocket> webSocket) {
			int ws = emplaceWebSocket(webSocket);
			if (auto ptr = getUserPointer(wsserver)) {
				rtcSetUserPointer(wsserver, *ptr);
				cb(wsserver, ws, *ptr);
			}
		});

		return wsserver;
	});
}

RTC_C_EXPORT int rtcDeleteWebSocketServer(int wsserver) {
	return wrap([&] {
		auto webSocketServer = getWebSocketServer(wsserver);
		webSocketServer->onClient(nullptr);
		webSocketServer->stop();
		eraseWebSocketServer(wsserver);
		return RTC_ERR_SUCCESS;
	});
}

RTC_C_EXPORT int rtcGetWebSocketServerPort(int wsserver) {
	return wrap([&] {
		auto webSocketServer = getWebSocketServer(wsserver);
		return int(webSocketServer->port());
	});
}

#endif

void rtcPreload() {
	try {
		rtc::Preload();
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}
}

void rtcCleanup() {
	try {
		size_t count = eraseAll();
		if (count != 0) {
			PLOG_INFO << count << " objects were not properly destroyed before cleanup";
		}

		if (rtc::Cleanup().wait_for(10s) == std::future_status::timeout)
			throw std::runtime_error(
			    "Cleanup timeout (possible deadlock or undestructible object)");

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}
}

int rtcSetSctpSettings(const rtcSctpSettings *settings) {
	return wrap([&] {
		SctpSettings s = {};

		if (settings->recvBufferSize > 0)
			s.recvBufferSize = size_t(settings->recvBufferSize);

		if (settings->sendBufferSize > 0)
			s.sendBufferSize = size_t(settings->sendBufferSize);

		if (settings->maxChunksOnQueue > 0)
			s.maxChunksOnQueue = size_t(settings->maxChunksOnQueue);

		if (settings->initialCongestionWindow > 0)
			s.initialCongestionWindow = size_t(settings->initialCongestionWindow);

		if (settings->maxBurst > 0)
			s.maxBurst = size_t(settings->maxBurst);
		else if (settings->maxBurst < 0)
			s.maxBurst = size_t(0); // setting to 0 disables, not setting chooses optimized default

		if (settings->congestionControlModule >= 0)
			s.congestionControlModule = unsigned(settings->congestionControlModule);

		if (settings->delayedSackTimeMs > 0)
			s.delayedSackTime = milliseconds(settings->delayedSackTimeMs);
		else if (settings->delayedSackTimeMs < 0)
			s.delayedSackTime = milliseconds(0);

		if (settings->minRetransmitTimeoutMs > 0)
			s.minRetransmitTimeout = milliseconds(settings->minRetransmitTimeoutMs);

		if (settings->maxRetransmitTimeoutMs > 0)
			s.maxRetransmitTimeout = milliseconds(settings->maxRetransmitTimeoutMs);

		if (settings->initialRetransmitTimeoutMs > 0)
			s.initialRetransmitTimeout = milliseconds(settings->initialRetransmitTimeoutMs);

		if (settings->maxRetransmitAttempts > 0)
			s.maxRetransmitAttempts = settings->maxRetransmitAttempts;

		if (settings->heartbeatIntervalMs > 0)
			s.heartbeatInterval = milliseconds(settings->heartbeatIntervalMs);

		SetSctpSettings(std::move(s));
		return RTC_ERR_SUCCESS;
	});
}
