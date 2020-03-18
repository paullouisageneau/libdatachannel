/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "include.hpp"

#include "datachannel.hpp"
#include "peerconnection.hpp"

#if RTC_ENABLE_WEBSOCKET
#include "websocket.hpp"
#endif

#include <rtc.h>

#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>

using namespace rtc;
using std::shared_ptr;
using std::string;

#define CATCH(statement)                                                                           \
	try {                                                                                          \
		statement;                                                                                 \
	} catch (const std::exception &e) {                                                            \
		PLOG_ERROR << e.what();                                                                    \
		return -1;                                                                                 \
	}

namespace {

std::unordered_map<int, shared_ptr<PeerConnection>> peerConnectionMap;
std::unordered_map<int, shared_ptr<DataChannel>> dataChannelMap;
#if RTC_ENABLE_WEBSOCKET
std::unordered_map<int, shared_ptr<WebSocket>> webSocketMap;
#endif
std::unordered_map<int, void *> userPointerMap;
std::mutex mutex;
int lastId = 0;

void *getUserPointer(int id) {
	std::lock_guard lock(mutex);
	auto it = userPointerMap.find(id);
	return it != userPointerMap.end() ? it->second : nullptr;
}

void setUserPointer(int i, void *ptr) {
	std::lock_guard lock(mutex);
	if (ptr)
		userPointerMap.insert(std::make_pair(i, ptr));
	else
		userPointerMap.erase(i);
}

shared_ptr<PeerConnection> getPeerConnection(int id) {
	std::lock_guard lock(mutex);
	auto it = peerConnectionMap.find(id);
	return it != peerConnectionMap.end() ? it->second : nullptr;
}

shared_ptr<DataChannel> getDataChannel(int id) {
	std::lock_guard lock(mutex);
	auto it = dataChannelMap.find(id);
	return it != dataChannelMap.end() ? it->second : nullptr;
}

int emplacePeerConnection(shared_ptr<PeerConnection> ptr) {
	std::lock_guard lock(mutex);
	int pc = ++lastId;
	peerConnectionMap.emplace(std::make_pair(pc, ptr));
	return pc;
}

int emplaceDataChannel(shared_ptr<DataChannel> ptr) {
	std::lock_guard lock(mutex);
	int dc = ++lastId;
	dataChannelMap.emplace(std::make_pair(dc, ptr));
	return dc;
}

bool erasePeerConnection(int pc) {
	std::lock_guard lock(mutex);
	if (peerConnectionMap.erase(pc) == 0)
		return false;
	userPointerMap.erase(pc);
	return true;
}

bool eraseDataChannel(int dc) {
	std::lock_guard lock(mutex);
	if (dataChannelMap.erase(dc) == 0)
		return false;
	userPointerMap.erase(dc);
	return true;
}

#if RTC_ENABLE_WEBSOCKET
shared_ptr<WebSocket> getWebSocket(int id) {
	std::lock_guard lock(mutex);
	auto it = webSocketMap.find(id);
	return it != webSocketMap.end() ? it->second : nullptr;
}

int emplaceWebSocket(shared_ptr<WebSocket> ptr) {
	std::lock_guard lock(mutex);
	int ws = ++lastId;
	webSocketMap.emplace(std::make_pair(ws, ptr));
	return ws;
}

bool eraseWebSocket(int ws) {
	std::lock_guard lock(mutex);
	if (webSocketMap.erase(ws) == 0)
		return false;
	userPointerMap.erase(ws);
	return true;
}
#endif

shared_ptr<Channel> getChannel(int id) {
	std::lock_guard lock(mutex);
	if (auto it = dataChannelMap.find(id); it != dataChannelMap.end())
		return it->second;
#if RTC_ENABLE_WEBSOCKET
	if (auto it = webSocketMap.find(id); it != webSocketMap.end())
		return it->second;
#endif
	return nullptr;
}

} // namespace

void rtcInitLogger(rtcLogLevel level) { InitLogger(static_cast<LogLevel>(level)); }

void rtcSetUserPointer(int i, void *ptr) { setUserPointer(i, ptr); }

int rtcCreatePeerConnection(const rtcConfiguration *config) {
	Configuration c;
	for (int i = 0; i < config->iceServersCount; ++i)
		c.iceServers.emplace_back(string(config->iceServers[i]));

	if (config->portRangeBegin || config->portRangeEnd) {
		c.portRangeBegin = config->portRangeBegin;
		c.portRangeEnd = config->portRangeEnd;
	}

	return emplacePeerConnection(std::make_shared<PeerConnection>(c));
}

int rtcDeletePeerConnection(int pc) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	peerConnection->onDataChannel(nullptr);
	peerConnection->onLocalDescription(nullptr);
	peerConnection->onLocalCandidate(nullptr);
	peerConnection->onStateChange(nullptr);
	peerConnection->onGatheringStateChange(nullptr);

	erasePeerConnection(pc);
	return 0;
}

int rtcCreateDataChannel(int pc, const char *label) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	int dc = emplaceDataChannel(peerConnection->createDataChannel(string(label)));
	void *ptr = getUserPointer(pc);
	rtcSetUserPointer(dc, ptr);
	return dc;
}

int rtcDeleteDataChannel(int dc) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	dataChannel->onOpen(nullptr);
	dataChannel->onClosed(nullptr);
	dataChannel->onError(nullptr);
	dataChannel->onMessage(nullptr);
	dataChannel->onBufferedAmountLow(nullptr);
	dataChannel->onAvailable(nullptr);

	eraseDataChannel(dc);
	return 0;
}

#if RTC_ENABLE_WEBSOCKET
int rtcCreateWebSocket(const char *url) {
	return emplaceWebSocket(std::make_shared<WebSocket>(url));
}

int rtcDeleteWebsocket(int ws) {
	auto webSocket = getWebSocket(ws);
	if (!webSocket)
		return -1;

	webSocket->onOpen(nullptr);
	webSocket->onClosed(nullptr);
	webSocket->onError(nullptr);
	webSocket->onMessage(nullptr);
	webSocket->onBufferedAmountLow(nullptr);
	webSocket->onAvailable(nullptr);

	eraseWebSocket(ws);
	return 0;
}

#endif

int rtcSetDataChannelCallback(int pc, dataChannelCallbackFunc cb) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (cb)
		peerConnection->onDataChannel([pc, cb](std::shared_ptr<DataChannel> dataChannel) {
			int dc = emplaceDataChannel(dataChannel);
			void *ptr = getUserPointer(pc);
			rtcSetUserPointer(dc, ptr);
			cb(dc, ptr);
		});
	else
		peerConnection->onDataChannel(nullptr);
	return 0;
}

int rtcSetLocalDescriptionCallback(int pc, descriptionCallbackFunc cb) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (cb)
		peerConnection->onLocalDescription([pc, cb](const Description &desc) {
			cb(string(desc).c_str(), desc.typeString().c_str(), getUserPointer(pc));
		});
	else
		peerConnection->onLocalDescription(nullptr);
	return 0;
}

int rtcSetLocalCandidateCallback(int pc, candidateCallbackFunc cb) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (cb)
		peerConnection->onLocalCandidate([pc, cb](const Candidate &cand) {
			cb(cand.candidate().c_str(), cand.mid().c_str(), getUserPointer(pc));
		});
	else
		peerConnection->onLocalCandidate(nullptr);
	return 0;
}

int rtcSetStateChangeCallback(int pc, stateChangeCallbackFunc cb) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (cb)
		peerConnection->onStateChange([pc, cb](PeerConnection::State state) {
			cb(static_cast<rtcState>(state), getUserPointer(pc));
		});
	else
		peerConnection->onStateChange(nullptr);
	return 0;
}

int rtcSetGatheringStateChangeCallback(int pc, gatheringStateCallbackFunc cb) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (cb)
		peerConnection->onGatheringStateChange([pc, cb](PeerConnection::GatheringState state) {
			cb(static_cast<rtcGatheringState>(state), getUserPointer(pc));
		});
	else
		peerConnection->onGatheringStateChange(nullptr);
	return 0;
}

int rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	CATCH(peerConnection->setRemoteDescription({string(sdp), type ? string(type) : ""}));
	return 0;
}

int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	CATCH(peerConnection->addRemoteCandidate({string(cand), mid ? string(mid) : ""}))
	return 0;
}

int rtcGetLocalAddress(int pc, char *buffer, int size) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (auto addr = peerConnection->localAddress()) {
		size = std::min(size_t(size - 1), addr->size());
		std::copy(addr->data(), addr->data() + size, buffer);
		buffer[size] = '\0';
		return size + 1;
	}
	return -1;
}

int rtcGetRemoteAddress(int pc, char *buffer, int size) {
	auto peerConnection = getPeerConnection(pc);
	if (!peerConnection)
		return -1;

	if (auto addr = peerConnection->remoteAddress()) {
		size = std::min(size_t(size - 1), addr->size());
		std::copy(addr->data(), addr->data() + size, buffer);
		buffer[size] = '\0';
		return size + 1;
	}
	return -1;
}

int rtcGetDataChannelLabel(int dc, char *buffer, int size) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (!size)
		return 0;

	string label = dataChannel->label();
	size = std::min(size_t(size - 1), label.size());
	std::copy(label.data(), label.data() + size, buffer);
	buffer[size] = '\0';
	return size + 1;
}

int rtcSetOpenCallback(int id, openCallbackFunc cb) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (cb)
		channel->onOpen([id, cb]() { cb(getUserPointer(id)); });
	else
		channel->onOpen(nullptr);
	return 0;
}

int rtcSetClosedCallback(int id, closedCallbackFunc cb) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (cb)
		channel->onClosed([id, cb]() { cb(getUserPointer(id)); });
	else
		channel->onClosed(nullptr);
	return 0;
}

int rtcSetErrorCallback(int id, errorCallbackFunc cb) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (cb)
		channel->onError([id, cb](const string &error) { cb(error.c_str(), getUserPointer(id)); });
	else
		channel->onError(nullptr);
	return 0;
}

int rtcSetMessageCallback(int id, messageCallbackFunc cb) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (cb)
		channel->onMessage(
		    [id, cb](const binary &b) {
			    cb(reinterpret_cast<const char *>(b.data()), b.size(), getUserPointer(id));
		    },
		    [id, cb](const string &s) { cb(s.c_str(), -1, getUserPointer(id)); });
	else
		channel->onMessage(nullptr);

	return 0;
}

int rtcSendMessage(int id, const char *data, int size) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (size >= 0) {
		auto b = reinterpret_cast<const byte *>(data);
		CATCH(channel->send(binary(b, b + size)));
		return size;
	} else {
		string str(data);
		int len = str.size();
		CATCH(channel->send(std::move(str)));
		return len;
	}
}

int rtcGetBufferedAmount(int id) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	CATCH(return int(channel->bufferedAmount()));
}

int rtcSetBufferedAmountLowThreshold(int id, int amount) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	CATCH(channel->setBufferedAmountLowThreshold(size_t(amount)));
	return 0;
}

int rtcSetBufferedAmountLowCallback(int id, bufferedAmountLowCallbackFunc cb) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (cb)
		channel->onBufferedAmountLow([id, cb]() { cb(getUserPointer(id)); });
	else
		channel->onBufferedAmountLow(nullptr);
	return 0;
}

int rtcGetAvailableAmount(int id) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	CATCH(return int(channel->availableAmount()));
}

int rtcSetAvailableCallback(int id, availableCallbackFunc cb) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (cb)
		channel->onOpen([id, cb]() { cb(getUserPointer(id)); });
	else
		channel->onOpen(nullptr);
	return 0;
}

int rtcReceiveMessage(int id, char *buffer, int *size) {
	auto channel = getChannel(id);
	if (!channel)
		return -1;

	if (!size)
		return -1;

	CATCH({
		auto message = channel->receive();
		if (!message)
			return 0;

		return std::visit( //
		    overloaded{    //
		               [&](const binary &b) {
			               *size = std::min(*size, int(b.size()));
			               auto data = reinterpret_cast<const char *>(b.data());
			               std::copy(data, data + *size, buffer);
			               return *size;
		               },
		               [&](const string &s) {
			               int len = std::min(*size - 1, int(s.size()));
			               if (len >= 0) {
				               std::copy(s.data(), s.data() + len, buffer);
				               buffer[len] = '\0';
			               }
			               *size = -(len + 1);
			               return len + 1;
		               }},
		    *message);
	});
}
