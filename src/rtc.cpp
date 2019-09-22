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
#include "peerconnection.hpp"

#include <rtc.h>

#include <unordered_map>

using namespace rtc;
using std::shared_ptr;
using std::string;

namespace {

std::unordered_map<int, shared_ptr<PeerConnection>> peerConnectionMap;
std::unordered_map<int, shared_ptr<DataChannel>> dataChannelMap;
std::unordered_map<int, void *> userPointerMap;
int lastId = 0;

void *getUserPointer(int id) {
	auto it = userPointerMap.find(id);
	return it != userPointerMap.end() ? it->second : nullptr;
}

} // namespace

int rtcCreatePeerConnection(const char **iceServers, int iceServersCount) {
	Configuration config;
	for (int i = 0; i < iceServersCount; ++i) {
		config.iceServers.emplace_back(IceServer(string(iceServers[i])));
	}
	int pc = ++lastId;
	peerConnectionMap.emplace(std::make_pair(pc, std::make_shared<PeerConnection>(config)));
	return pc;
}

void rtcDeletePeerConnection(int pc) { peerConnectionMap.erase(pc); }

int rtcCreateDataChannel(int pc, const char *label) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return 0;
	auto dataChannel = it->second->createDataChannel(string(label));
	int dc = ++lastId;
	dataChannelMap.emplace(std::make_pair(dc, dataChannel));
	return dc;
}

void rtcDeleteDataChannel(int dc) { dataChannelMap.erase(dc); }

void rtcSetDataChannelCallback(int pc, void (*dataChannelCallback)(int, void *)) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->onDataChannel([pc, dataChannelCallback](std::shared_ptr<DataChannel> dataChannel) {
		int dc = ++lastId;
		dataChannelMap.emplace(std::make_pair(dc, dataChannel));
		dataChannelCallback(dc, getUserPointer(pc));
	});
}

void rtcSetLocalDescriptionCallback(int pc, void (*descriptionCallback)(const char *, const char *,
                                                                        void *)) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->onLocalDescription([pc, descriptionCallback](const Description &description) {
		descriptionCallback(string(description).c_str(), description.typeString().c_str(),
		                    getUserPointer(pc));
	});
}

void rtcSetLocalCandidateCallback(int pc,
                                  void (*candidateCallback)(const char *, const char *, void *)) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->onLocalCandidate([pc, candidateCallback](const Candidate &candidate) {
		candidateCallback(candidate.candidate().c_str(), candidate.mid().c_str(),
		                  getUserPointer(pc));
	});
}

void rtcSetStateChangedCallback(int pc, void (*stateCallback)(rtc_state_t state, void *)) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->onStateChanged([pc, stateCallback](PeerConnection::State state) {
		stateCallback(static_cast<rtc_state_t>(state), getUserPointer(pc));
	});
}

void rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->setRemoteDescription(Description(string(sdp), type ? string(type) : ""));
}

void rtcAddRemoteCandidate(int pc, const char *candidate, const char *mid) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->addRemoteCandidate(Candidate(string(candidate), mid ? string(mid) : ""));
}

int rtcGetDataChannelLabel(int dc, char *buffer, int size) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return 0;

	if (!size)
		return 0;

	string label = it->second->label();
	size = std::min(size_t(size - 1), label.size());
	std::copy(label.data(), label.data() + size, buffer);
	buffer[size] = '\0';
	return size + 1;
}

void rtcSetOpenCallback(int dc, void (*openCallback)(void *)) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return;

	it->second->onOpen([dc, openCallback]() { openCallback(getUserPointer(dc)); });
}

void rtcSetErrorCallback(int dc, void (*errorCallback)(const char *, void *)) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return;

	it->second->onError([dc, errorCallback](const string &error) {
		errorCallback(error.c_str(), getUserPointer(dc));
	});
}

void rtcSetMessageCallback(int dc, void (*messageCallback)(const char *, int, void *)) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return;

	it->second->onMessage(
	    [dc, messageCallback](const binary &b) {
		    messageCallback(reinterpret_cast<const char *>(b.data()), b.size(), getUserPointer(dc));
	    },
	    [dc, messageCallback](const string &s) {
		    messageCallback(s.c_str(), -1, getUserPointer(dc));
	    });
}

int rtcSendMessage(int dc, const char *data, int size) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return 0;

	if (size >= 0) {
		auto b = reinterpret_cast<const byte *>(data);
		it->second->send(b, size);
		return size;
	} else {
		string s(data);
		it->second->send(s);
		return s.size();
	}
}

void rtcSetUserPointer(int i, void *ptr) {
	if (ptr)
		userPointerMap.insert(std::make_pair(i, ptr));
	else
		userPointerMap.erase(i);
}

