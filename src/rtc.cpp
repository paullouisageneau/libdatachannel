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
		void *userPointer = nullptr;
		if (auto jt = userPointerMap.find(pc); jt != userPointerMap.end())
			userPointer = jt->second;
		dataChannelCallback(dc, userPointer);
	});
}

void rtcSetLocalDescriptionCallback(int pc, void (*descriptionCallback)(const char *, const char *,
                                                                        void *)) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->onLocalDescription([pc, descriptionCallback](const Description &description) {
		void *userPointer = nullptr;
		if (auto jt = userPointerMap.find(pc); jt != userPointerMap.end())
			userPointer = jt->second;
		string type = description.type() == Description::Type::Answer ? "answer" : "offer";
		descriptionCallback(string(description).c_str(), type.c_str(), userPointer);
	});
}

void rtcSetLocalCandidateCallback(int pc,
                                  void (*candidateCallback)(const char *, const char *, void *)) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->onLocalCandidate(
	    [pc, candidateCallback](const std::optional<Candidate> &candidate) {
		    void *userPointer = nullptr;
		    if (auto jt = userPointerMap.find(pc); jt != userPointerMap.end())
			    userPointer = jt->second;
		    if (candidate) {
			    auto mid = candidate->mid() ? *candidate->mid() : string();
			    candidateCallback(candidate->candidate().c_str(), mid.c_str(), userPointer);
		    } else {
			    candidateCallback(nullptr, nullptr, userPointer);
		    }
	    });
}

void rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	auto t =
	    type && string(type) == "answer" ? Description::Type::Answer : Description::Type::Offer;
	it->second->setRemoteDescription(Description(string(sdp), t));
}

void rtcAddRemoteCandidate(int pc, const char *candidate, const char *mid) {
	auto it = peerConnectionMap.find(pc);
	if (it == peerConnectionMap.end())
		return;

	it->second->addRemoteCandidate(
	    Candidate(string(candidate), mid ? make_optional(string(mid)) : nullopt));
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

	it->second->onOpen([dc, openCallback]() {
		void *userPointer = nullptr;
		if (auto jt = userPointerMap.find(dc); jt != userPointerMap.end())
			userPointer = jt->second;
		openCallback(userPointer);
	});
}

void rtcSetErrorCallback(int dc, void (*errorCallback)(const char *, void *)) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return;

	it->second->onError([dc, errorCallback](const string &error) {
		void *userPointer = nullptr;
		if (auto jt = userPointerMap.find(dc); jt != userPointerMap.end())
			userPointer = jt->second;
		errorCallback(error.c_str(), userPointer);
	});
}

void rtcSetMessageCallback(int dc, void (*messageCallback)(const char *, int, void *)) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return;

	it->second->onMessage([dc, messageCallback](const std::variant<binary, string> &message) {
		void *userPointer = nullptr;
		if (auto jt = userPointerMap.find(dc); jt != userPointerMap.end())
			userPointer = jt->second;
		std::visit(
		    [messageCallback, userPointer](const auto &v) {
			    auto data = reinterpret_cast<const char *>(v.data());
			    int size = v.size();
			    messageCallback(data, size, userPointer);
		    },
		    message);
	});
}

int rtcSendMessage(int dc, const char *data, int size) {
	auto it = dataChannelMap.find(dc);
	if (it == dataChannelMap.end())
		return 0;

	auto b = reinterpret_cast<const byte *>(data);
	it->second->send(b, size);
	return size;
}

void rtcSetUserPointer(int i, void *ptr) {
	if (ptr)
		userPointerMap.insert(std::make_pair(i, ptr));
	else
		userPointerMap.erase(i);
}

