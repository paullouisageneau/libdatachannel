/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
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

#include "rtc.h"

#include "datachannel.hpp"
#include "log.hpp"
#include "peerconnection.hpp"
#if RTC_ENABLE_WEBSOCKET
#include "websocket.hpp"
#endif

#include "plog/Formatters/FuncMessageFormatter.h"

#include <chrono>
#include <exception>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <codecvt>
#include <locale>
#endif

using namespace rtc;
using std::shared_ptr;
using std::string;
using std::chrono::milliseconds;

namespace {

std::unordered_map<int, shared_ptr<PeerConnection>> peerConnectionMap;
std::unordered_map<int, shared_ptr<DataChannel>> dataChannelMap;
std::unordered_map<int, shared_ptr<Track>> trackMap;
#if RTC_ENABLE_WEBSOCKET
std::unordered_map<int, shared_ptr<WebSocket>> webSocketMap;
#endif
std::unordered_map<int, void *> userPointerMap;
std::mutex mutex;
int lastId = 0;

std::optional<void *> getUserPointer(int id) {
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
		throw std::invalid_argument("PeerConnection ID does not exist");
	userPointerMap.erase(pc);
}

void eraseDataChannel(int dc) {
	std::lock_guard lock(mutex);
	if (dataChannelMap.erase(dc) == 0)
		throw std::invalid_argument("DataChannel ID does not exist");
	userPointerMap.erase(dc);
}

void eraseTrack(int tr) {
	std::lock_guard lock(mutex);
	if (trackMap.erase(tr) == 0)
		throw std::invalid_argument("Track ID does not exist");
	userPointerMap.erase(tr);
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
	userPointerMap.emplace(std::make_pair(ws, nullptr));
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
	if (auto it = trackMap.find(id); it != trackMap.end())
		return it->second;
#if RTC_ENABLE_WEBSOCKET
	if (auto it = webSocketMap.find(id); it != webSocketMap.end())
		return it->second;
#endif
	throw std::invalid_argument("DataChannel, Track, or WebSocket ID does not exist");
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

#define WRAP(statement)                                                                            \
	wrap([&]() {                                                                                   \
		statement;                                                                                 \
		return RTC_ERR_SUCCESS;                                                                    \
	})

class plog_appender : public plog::IAppender {
public:
	plog_appender(rtcLogCallbackFunc cb = nullptr) { set_callback(cb); }

	void set_callback(rtcLogCallbackFunc cb) {
		std::lock_guard lock(mutex);
		callback = cb;
	}

	void write(const plog::Record &record) override {
		plog::Severity severity = record.getSeverity();
		auto formatted = plog::FuncMessageFormatter::format(record);
		formatted.pop_back(); // remove newline
#ifdef _WIN32
		using convert_type = std::codecvt_utf8<wchar_t>;
		std::wstring_convert<convert_type, wchar_t> converter;
		std::string str = converter.to_bytes(formatted);
#else
		std::string str = formatted;
#endif
		std::lock_guard lock(mutex);
		if (callback)
			callback(static_cast<rtcLogLevel>(record.getSeverity()), str.c_str());
		else
			std::cout << plog::severityToString(severity) << " " << str << std::endl;
	}

private:
	rtcLogCallbackFunc callback;
};

} // namespace

void rtcInitLogger(rtcLogLevel level, rtcLogCallbackFunc cb) {
	static std::optional<plog_appender> appender;
	if (appender)
		appender->set_callback(cb);
	else if (cb)
		appender.emplace(plog_appender(cb));

	InitLogger(static_cast<plog::Severity>(level), appender ? &appender.value() : nullptr);
}

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
	return rtcCreateDataChannelExt(pc, label, nullptr, nullptr);
}

int rtcCreateDataChannelExt(int pc, const char *label, const char *protocol,
                            const rtcReliability *reliability) {
	return WRAP({
		Reliability r = {};
		if (reliability) {
			r.unordered = reliability->unordered;
			if (reliability->unreliable) {
				if (reliability->maxPacketLifeTime > 0) {
					r.type = Reliability::Type::Timed;
					r.rexmit = milliseconds(reliability->maxPacketLifeTime);
				} else {
					r.type = Reliability::Type::Rexmit;
					r.rexmit = int(reliability->maxRetransmits);
				}
			} else {
				r.type = Reliability::Type::Reliable;
			}
		}
		auto peerConnection = getPeerConnection(pc);
		int dc = emplaceDataChannel(peerConnection->createDataChannel(
		    string(label ? label : ""), string(protocol ? protocol : ""), r));
		if (auto ptr = getUserPointer(pc))
			rtcSetUserPointer(dc, *ptr);
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

int rtcCreateTrack(int pc, const char *mediaDescriptionSdp) {
	if (!mediaDescriptionSdp)
		throw std::invalid_argument("Unexpected null pointer for track media description");

	auto peerConnection = getPeerConnection(pc);
	Description::Media media{string(mediaDescriptionSdp)};
	int tr = emplaceTrack(peerConnection->createTrack(std::move(media)));
	if (auto ptr = getUserPointer(pc))
		rtcSetUserPointer(tr, *ptr);
	return tr;
}

int rtcDeleteTrack(int tr) {
	return WRAP({
		auto track = getTrack(tr);
		track->onOpen(nullptr);
		track->onClosed(nullptr);
		track->onError(nullptr);
		track->onMessage(nullptr);
		track->onBufferedAmountLow(nullptr);
		track->onAvailable(nullptr);

		eraseTrack(tr);
	});
}

int rtcGetTrackDescription(int tr, char *buffer, int size) {
	return WRAP({
		auto track = getTrack(tr);

		if (size <= 0)
			return 0;

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer for buffer");

		string description(track->description());
		const char *data = description.data();
		size = std::min(size - 1, int(description.size()));
		std::copy(data, data + size, buffer);
		buffer[size] = '\0';
		return int(size + 1);
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

int rtcCreateWebSocketEx(const char *url, const rtcWsConfiguration *config) {
	return WRAP({
		WebSocket::Configuration c;
		c.disableTlsVerification = config->disableTlsVerification;
		auto ws = std::make_shared<WebSocket>(c);
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

int rtcSetLocalDescriptionCallback(int pc, rtcDescriptionCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onLocalDescription([pc, cb](Description desc) {
				if (auto ptr = getUserPointer(pc))
					cb(string(desc).c_str(), desc.typeString().c_str(), *ptr);
			});
		else
			peerConnection->onLocalDescription(nullptr);
	});
}

int rtcSetLocalCandidateCallback(int pc, rtcCandidateCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onLocalCandidate([pc, cb](Candidate cand) {
				if (auto ptr = getUserPointer(pc))
					cb(cand.candidate().c_str(), cand.mid().c_str(), *ptr);
			});
		else
			peerConnection->onLocalCandidate(nullptr);
	});
}

int rtcSetStateChangeCallback(int pc, rtcStateChangeCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onStateChange([pc, cb](PeerConnection::State state) {
				if (auto ptr = getUserPointer(pc))
					cb(static_cast<rtcState>(state), *ptr);
			});
		else
			peerConnection->onStateChange(nullptr);
	});
}

int rtcSetGatheringStateChangeCallback(int pc, rtcGatheringStateCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onGatheringStateChange([pc, cb](PeerConnection::GatheringState state) {
				if (auto ptr = getUserPointer(pc))
					cb(static_cast<rtcGatheringState>(state), *ptr);
			});
		else
			peerConnection->onGatheringStateChange(nullptr);
	});
}

int rtcSetDataChannelCallback(int pc, rtcDataChannelCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onDataChannel([pc, cb](std::shared_ptr<DataChannel> dataChannel) {
				int dc = emplaceDataChannel(dataChannel);
				if (auto ptr = getUserPointer(pc)) {
					rtcSetUserPointer(dc, *ptr);
					cb(dc, *ptr);
				}
			});
		else
			peerConnection->onDataChannel(nullptr);
	});
}

int rtcSetTrackCallback(int pc, rtcTrackCallbackFunc cb) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);
		if (cb)
			peerConnection->onTrack([pc, cb](std::shared_ptr<Track> track) {
				int tr = emplaceTrack(track);
				if (auto ptr = getUserPointer(pc)) {
					rtcSetUserPointer(tr, *ptr);
					cb(tr, *ptr);
				}
			});
		else
			peerConnection->onTrack(nullptr);
	});
}

int rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (!sdp)
			throw std::invalid_argument("Unexpected null pointer for remote description");

		peerConnection->setRemoteDescription({string(sdp), type ? string(type) : ""});
	});
}

int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (!cand)
			throw std::invalid_argument("Unexpected null pointer for remote candidate");

		peerConnection->addRemoteCandidate({string(cand), mid ? string(mid) : ""});
	});
}

int rtcGetLocalAddress(int pc, char *buffer, int size) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (size <= 0)
			return 0;

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer for buffer");

		if (auto addr = peerConnection->localAddress()) {
			const char *data = addr->data();
			size = std::min(size - 1, int(addr->size()));
			std::copy(data, data + size, buffer);
			buffer[size] = '\0';
			return size + 1;
		}
	});
}

int rtcGetRemoteAddress(int pc, char *buffer, int size) {
	return WRAP({
		auto peerConnection = getPeerConnection(pc);

		if (size <= 0)
			return 0;

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer for buffer");

		if (auto addr = peerConnection->remoteAddress()) {
			const char *data = addr->data();
			size = std::min(size - 1, int(addr->size()));
			std::copy(data, data + size, buffer);
			buffer[size] = '\0';
			return int(size + 1);
		}
	});
}

int rtcGetDataChannelLabel(int dc, char *buffer, int size) {
	return WRAP({
		auto dataChannel = getDataChannel(dc);

		if (size <= 0)
			return 0;

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer for buffer");

		string label = dataChannel->label();
		const char *data = label.data();
		size = std::min(size - 1, int(label.size()));
		std::copy(data, data + size, buffer);
		buffer[size] = '\0';
		return int(size + 1);
	});
}

int rtcGetDataChannelProtocol(int dc, char *buffer, int size) {
	return WRAP({
		auto dataChannel = getDataChannel(dc);

		if (size <= 0)
			return 0;

		if (!buffer)
			throw std::invalid_argument("Unexpected null pointer for buffer");

		string protocol = dataChannel->protocol();
		const char *data = protocol.data();
		size = std::min(size - 1, int(protocol.size()));
		std::copy(data, data + size, buffer);
		buffer[size] = '\0';
		return int(size + 1);
	});
}

int rtcGetDataChannelReliability(int dc, rtcReliability *reliability) {
	return WRAP({
		auto dataChannel = getDataChannel(dc);

		if (!reliability)
			throw std::invalid_argument("Unexpected null pointer for reliability");

		Reliability r = dataChannel->reliability();
		std::memset(reliability, 0, sizeof(*reliability));
		reliability->unordered = r.unordered;
		if (r.type == Reliability::Type::Timed) {
			reliability->unreliable = true;
			reliability->maxPacketLifeTime = unsigned(std::get<milliseconds>(r.rexmit).count());
		} else if (r.type == Reliability::Type::Rexmit) {
			reliability->unreliable = true;
			reliability->maxRetransmits = unsigned(std::get<int>(r.rexmit));
		} else {
			reliability->unreliable = false;
		}
		return 0;
	});
}

int rtcSetOpenCallback(int id, rtcOpenCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onOpen([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(*ptr);
			});
		else
			channel->onOpen(nullptr);
	});
}

int rtcSetClosedCallback(int id, rtcClosedCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onClosed([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(*ptr);
			});
		else
			channel->onClosed(nullptr);
	});
}

int rtcSetErrorCallback(int id, rtcErrorCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onError([id, cb](string error) {
				if (auto ptr = getUserPointer(id))
					cb(error.c_str(), *ptr);
			});
		else
			channel->onError(nullptr);
	});
}

int rtcSetMessageCallback(int id, rtcMessageCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onMessage(
			    [id, cb](binary b) {
				    if (auto ptr = getUserPointer(id))
					    cb(reinterpret_cast<const char *>(b.data()), int(b.size()), *ptr);
			    },
			    [id, cb](string s) {
				    if (auto ptr = getUserPointer(id))
					    cb(s.c_str(), -int(s.size() + 1), *ptr);
			    });
		else
			channel->onMessage(nullptr);
	});
}

int rtcSendMessage(int id, const char *data, int size) {
	return WRAP({
		auto channel = getChannel(id);

		if (!data && size != 0)
			throw std::invalid_argument("Unexpected null pointer for data");

		if (size >= 0) {
			auto b = reinterpret_cast<const byte *>(data);
			channel->send(binary(b, b + size));
			return size;
		} else {
			string str(data);
			int len = int(str.size());
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

int rtcSetBufferedAmountLowCallback(int id, rtcBufferedAmountLowCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onBufferedAmountLow([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(*ptr);
			});
		else
			channel->onBufferedAmountLow(nullptr);
	});
}

int rtcGetAvailableAmount(int id) {
	return WRAP({ return int(getChannel(id)->availableAmount()); });
}

int rtcSetAvailableCallback(int id, rtcAvailableCallbackFunc cb) {
	return WRAP({
		auto channel = getChannel(id);
		if (cb)
			channel->onOpen([id, cb]() {
				if (auto ptr = getUserPointer(id))
					cb(*ptr);
			});
		else
			channel->onOpen(nullptr);
	});
}

int rtcReceiveMessage(int id, char *buffer, int *size) {
	return WRAP({
		auto channel = getChannel(id);

		if (!size)
			throw std::invalid_argument("Unexpected null pointer for size");

		if (!buffer && *size != 0)
			throw std::invalid_argument("Unexpected null pointer for buffer");

		if (auto message = channel->receive())
			return std::visit( //
			    overloaded{    //
			               [&](binary b) {
				               if (*size > 0) {
					               *size = std::min(*size, int(b.size()));
					               auto data = reinterpret_cast<const char *>(b.data());
					               std::copy(data, data + *size, buffer);
				               }
				               return 1;
			               },
			               [&](string s) {
				               if (*size > 0) {
					               int len = std::min(*size - 1, int(s.size()));
					               if (len >= 0) {
						               std::copy(s.data(), s.data() + len, buffer);
						               buffer[len] = '\0';
					               }
					               *size = -(len + 1);
				               }
				               return 1;
			               }},
			    *message);
		else
			return 0;
	});
}

void rtcPreload() { rtc::Preload(); }
void rtcCleanup() { rtc::Cleanup(); }
