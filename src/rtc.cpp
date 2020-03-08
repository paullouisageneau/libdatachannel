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

#include "datachannel.hpp"
#include "include.hpp"
#include "peerconnection.hpp"

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
std::unordered_map<int, void *> userPointerMap;
std::mutex mutex;
int lastId = 0;

void *getUserPointer(int id) {
	std::lock_guard lock(mutex);
	auto it = userPointerMap.find(id);
	return it != userPointerMap.end() ? it->second : nullptr;
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

} // namespace

void rtcInitLogger(rtcLogLevel level) { InitLogger(static_cast<LogLevel>(level)); }

void rtcSetUserPointer(int i, void *ptr) {
	if (ptr)
		userPointerMap.insert(std::make_pair(i, ptr));
	else
		userPointerMap.erase(i);
}

int rtcCreatePeerConnection(const rtcConfiguration *config) {
	Configuration c;
	for (int i = 0; i < config->iceServersCount; ++i)
		c.iceServers.emplace_back(string(config->iceServers[i]));

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

int rtcSetOpenCallback(int dc, openCallbackFunc cb) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (cb)
		dataChannel->onOpen([dc, cb]() { cb(getUserPointer(dc)); });
	else
		dataChannel->onOpen(nullptr);
	return 0;
}

int rtcSetClosedCallback(int dc, closedCallbackFunc cb) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (cb)
		dataChannel->onClosed([dc, cb]() { cb(getUserPointer(dc)); });
	else
		dataChannel->onClosed(nullptr);
	return 0;
}

int rtcSetErrorCallback(int dc, errorCallbackFunc cb) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (cb)
		dataChannel->onError(
		    [dc, cb](const string &error) { cb(error.c_str(), getUserPointer(dc)); });
	else
		dataChannel->onError(nullptr);
	return 0;
}

int rtcSetMessageCallback(int dc, messageCallbackFunc cb) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (cb)
		dataChannel->onMessage(
		    [dc, cb](const binary &b) {
			    cb(reinterpret_cast<const char *>(b.data()), b.size(), getUserPointer(dc));
		    },
		    [dc, cb](const string &s) { cb(s.c_str(), -1, getUserPointer(dc)); });
	else
		dataChannel->onMessage(nullptr);

	return 0;
}

int rtcSendMessage(int dc, const char *data, int size) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (size >= 0) {
		auto b = reinterpret_cast<const byte *>(data);
		CATCH(dataChannel->send(b, size));
		return size;
	} else {
		string s(data);
		CATCH(dataChannel->send(s));
		return s.size();
	}
}

int rtcGetBufferedAmount(int dc) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	CATCH(return int(dataChannel->bufferedAmount()));
}

int rtcSetBufferedAmountLowThreshold(int dc, int amount) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	CATCH(dataChannel->setBufferedAmountLowThreshold(size_t(amount)));
	return 0;
}

int rtcSetBufferedAmountLowCallback(int dc, bufferedAmountLowCallbackFunc cb) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (cb)
		dataChannel->onBufferedAmountLow([dc, cb]() { cb(getUserPointer(dc)); });
	else
		dataChannel->onBufferedAmountLow(nullptr);
	return 0;
}

int rtcGetAvailableAmount(int dc) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	CATCH(return int(dataChannel->availableAmount()));
}

int rtcSetAvailableCallback(int dc, availableCallbackFunc cb) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (cb)
		dataChannel->onOpen([dc, cb]() { cb(getUserPointer(dc)); });
	else
		dataChannel->onOpen(nullptr);
	return 0;
}

int rtcReceiveMessage(int dc, char *buffer, int *size) {
	auto dataChannel = getDataChannel(dc);
	if (!dataChannel)
		return -1;

	if (!size)
		return -1;

	CATCH({
		auto message = dataChannel->receive();
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
