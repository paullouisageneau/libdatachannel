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
#include <type_traits>
#include <unordered_map>
#include <utility>

using namespace rtc;
using std::shared_ptr;
using std::string;

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

void erasePeerConnection(int pc) {
	std::lock_guard lock(mutex);
	if (peerConnectionMap.erase(pc) == 0)
		throw std::invalid_argument("PeerConnection ID does not exist");
	userPointerMap.erase(pc);
}

void eraseDataChannel(int dc) {
	std::lock_guard lock(mutex);
	if (dataChannelMap.erase(dc) == 0)
		throw std::invalid_argument("DataChannel ID does not exist");
	userPointerMap.erase(dc);
}

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
	return ws;
}

void eraseWebSocket(int ws) {
	std::lock_guard lock(mutex);
	if (webSocketMap.erase(ws) == 0)
		throw std::invalid_argument("WebSocket ID does not exist");
	userPointerMap.erase(ws);
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
	throw std::invalid_argument("DataChannel or WebSocket ID does not exist");
}

template <typename F> int wrap(F func) {
	try {
		return func();

	} catch (const std::invalid_argument &e) {
		PLOG_ERROR << e.what();
		return RTC_ERR_INVALID;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		return RTC_ERR_FAILURE;
	}
}

#define WRAP(statement)                                                                            \
	wrap([&]() {                                                                                   \
		statement;                                                                                 \
		return RTC_ERR_SUCCESS;                                                                    \
	})

} // namespace

void rtcInitLogger(rtcLogLevel level) { InitLogger(static_cast<LogLevel>(level)); }

void rtcSetUserPointer(int i, void *ptr) { setUserPointer(i, ptr); }

int rtcCreatePeerConnection(const rtcConfiguration *config) {
	return WRAP({
		Configuration c;
		for (int i = 0; i < config->iceServersCount; ++i)
			c.iceServers.emplace_back(string(config->iceServers[i]));

		if (config->portRangeBegin || config->portRangeEnd) {
			c.portRangeBegin = config->portRangeBegin;
			c.portRangeEnd = config->portRangeEnd;
		}

		return emplacePeerConnection(std::make_shared<PeerConnection>(c));
	});
}

int rtcDeletePeerConnection(int pc) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		peerConnection->onDataChannel(nullptr);
		peerConnection->onLocalDescription(nullptr);
		peerConnection->onLocalCandidate(nullptr);
		peerConnection->onStateChange(nullptr);
		peerConnection->onGatheringStateChange(nullptr);

		erasePeerConnection(pc);
	});
}

int rtcCreateDataChannel(int pc, const char *label) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		int dc = emplaceDataChannel(peerConnection->createDataChannel(string(label)));
		rtcSetUserPointer(dc, getUserPointer(pc));
		return dc;
	});
}

int rtcDeleteDataChannel(int dc) {
	return WRAP({
		auto dataChannel = getDataChannel(dc);
		dataChannel->onOpen(nullptr);
		dataChannel->onClosed(nullptr);
		dataChannel->onError(nullptr);
		dataChannel->onMessage(nullptr);
		dataChannel->onBufferedAmountLow(nullptr);
		dataChannel->onAvailable(nullptr);

		eraseDataChannel(dc);
	});
}

#if RTC_ENABLE_WEBSOCKET
int rtcCreateWebSocket(const char *url) {
	return WRAP({
		auto ws = std::make_shared<WebSocket>();
		ws->open(url);
		return emplaceWebSocket(ws);
	});
}

int rtcDeleteWebsocket(int ws) {
	return WRAP({
		auto webSocket = getWebSocket(ws);
		webSocket->onOpen(nullptr);
		webSocket->onClosed(nullptr);
		webSocket->onError(nullptr);
		webSocket->onMessage(nullptr);
		webSocket->onBufferedAmountLow(nullptr);
		webSocket->onAvailable(nullptr);

		eraseWebSocket(ws);
	});
}
#endif

int rtcSetDataChannelCallback(int pc, dataChannelCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onDataChannel([pc, cb](std::shared_ptr<DataChannel> dataChannel) {
				int dc = emplaceDataChannel(dataChannel);
				void *ptr = getUserPointer(pc);
				rtcSetUserPointer(dc, ptr);
				cb(dc, ptr);
			});
		else
			peerConnection->onDataChannel(nullptr);
	});
}

int rtcSetLocalDescriptionCallback(int pc, descriptionCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onLocalDescription([pc, cb](const Description &desc) {
				cb(string(desc).c_str(), desc.typeString().c_str(), getUserPointer(pc));
			});
		else
			peerConnection->onLocalDescription(nullptr);
	});
}

int rtcSetLocalCandidateCallback(int pc, candidateCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onLocalCandidate([pc, cb](const Candidate &cand) {
				cb(cand.candidate().c_str(), cand.mid().c_str(), getUserPointer(pc));
			});
		else
			peerConnection->onLocalCandidate(nullptr);
	});
}

int rtcSetStateChangeCallback(int pc, stateChangeCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onStateChange([pc, cb](PeerConnection::State state) {
				cb(static_cast<rtcState>(state), getUserPointer(pc));
			});
		else
			peerConnection->onStateChange(nullptr);
	});
}

int rtcSetGatheringStateChangeCallback(int pc, gatheringStateCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onGatheringStateChange([pc, cb](PeerConnection::GatheringState state) {
				cb(static_cast<rtcGatheringState>(state), getUserPointer(pc));
			});
		else
			peerConnection->onGatheringStateChange(nullptr);
	});
}

int rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (!sdp)
			throw std::invalid_argument("Unexpected null pointer");

		peerConnection->setRemoteDescription({string(sdp), type ? string(type) : ""});
	});
}

int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (!cand)
			throw std::invalid_argument("Unexpected null pointer");

		peerConnection->addRemoteCandidate({string(cand), mid ? string(mid) : ""});
	});
}

int rtcGetLocalAddress(int pc, char *buffer, int size) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer");

		if (auto addr = peerConnection->localAddress()) {
			const char *data = addr->data();
			size = std::min(size_t(size - 1), addr->size());
			std::copy(data, data + size, buffer);
			buffer[size] = '\0';
			return size + 1;
		}
	});
}

int rtcGetRemoteAddress(int pc, char *buffer, int size) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer");

		if (auto addr = peerConnection->remoteAddress()) {
			const char *data = addr->data();
			size = std::min(size_t(size - 1), addr->size());
			std::copy(data, data + size, buffer);
			buffer[size] = '\0';
			return size + 1;
		}
	});
}

int rtcGetDataChannelLabel(int dc, char *buffer, int size) {
	return WRAP({
		auto dataChannel = getDataChannel(dc);

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer");

		if (size >= 0) {
			string label = dataChannel->label();
			const char *data = label.data();
			size = std::min(size_t(size - 1), label.size());
			std::copy(data, data + size, buffer);
			buffer[size] = '\0';
			return size + 1;
		} else {
			return 0;
		}
	});
}

int rtcSetOpenCallback(int id, openCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onOpen([id, cb]() { cb(getUserPointer(id)); });
		else
			channel->onOpen(nullptr);
	});
}

int rtcSetClosedCallback(int id, closedCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onClosed([id, cb]() { cb(getUserPointer(id)); });
		else
			channel->onClosed(nullptr);
	});
}

int rtcSetErrorCallback(int id, errorCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onError(
			    [id, cb](const string &error) { cb(error.c_str(), getUserPointer(id)); });
		else
			channel->onError(nullptr);
	});
}

int rtcSetMessageCallback(int id, messageCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onMessage(
			    [id, cb](const binary &b) {
				    cb(reinterpret_cast<const char *>(b.data()), b.size(), getUserPointer(id));
			    },
			    [id, cb](const string &s) { cb(s.c_str(), -1, getUserPointer(id)); });
		else
			channel->onMessage(nullptr);
	});
}

int rtcSendMessage(int id, const char *data, int size) {
	return WRAP({
		auto channel = getChannel(id);

		if (!data)
			throw std::invalid_argument("Unexpected null pointer");

		if (size >= 0) {
			auto b = reinterpret_cast<const byte *>(data);
			channel->send(binary(b, b + size));
			return size;
		} else {
			string str(data);
			int len = str.size();
			channel->send(std::move(str));
			return len;
		}
	});
}

int rtcGetBufferedAmount(int id) {
	return WRAP({
		auto channel = getChannel(id);
		return int(channel->bufferedAmount());
	});
}

int rtcSetBufferedAmountLowThreshold(int id, int amount) {
	return WRAP({
		auto channel = getChannel(id);
		channel->setBufferedAmountLowThreshold(size_t(amount));
	});
}

int rtcSetBufferedAmountLowCallback(int id, bufferedAmountLowCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onBufferedAmountLow([id, cb]() { cb(getUserPointer(id)); });
		else
			channel->onBufferedAmountLow(nullptr);
	});
}

int rtcGetAvailableAmount(int id) {
	return WRAP({ return int(getChannel(id)->availableAmount()); });
}

int rtcSetAvailableCallback(int id, availableCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onOpen([id, cb]() { cb(getUserPointer(id)); });
		else
			channel->onOpen(nullptr);
	});
}

int rtcReceiveMessage(int id, char *buffer, int *size) {
	return WRAP({
		auto channel = getChannel(id);

		if (!buffer || !size)
			throw std::invalid_argument("Unexpected null pointer");

		if (auto message = channel->receive())
			return std::visit( //
			    overloaded{    //
			               [&](const binary &b) {
				               *size = std::min(*size, int(b.size()));
				               auto data = reinterpret_cast<const char *>(b.data());
				               std::copy(data, data + *size, buffer);
				               return 1;
			               },
			               [&](const string &s) {
				               int len = std::min(*size - 1, int(s.size()));
				               if (len >= 0) {
					               std::copy(s.data(), s.data() + len, buffer);
					               buffer[len] = '\0';
				               }
				               *size = -(len + 1);
				               return 1;
			               }},
			    *message);
		else
			return 0;
	});
}

void rtcPreload() { rtc::Preload(); }
void rtcCleanup() { rtc::Cleanup(); }
