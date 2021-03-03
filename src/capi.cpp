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

#include "rtc.h"

#include "rtc.hpp"

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
using std::chrono::milliseconds;

namespace {

std::unordered_map<int, shared_ptr<PeerConnection>> peerConnectionMap;
std::unordered_map<int, shared_ptr<DataChannel>> dataChannelMap;
std::unordered_map<int, shared_ptr<Track>> trackMap;
#if RTC_ENABLE_MEDIA
std::unordered_map<int, shared_ptr<MediaChainableHandler>> rtcpChainableHandlerMap;
std::unordered_map<int, shared_ptr<RtcpSrReporter>> rtcpSrReporterMap;
std::unordered_map<int, shared_ptr<RtpPacketizationConfig>> rtpConfigMap;
#endif
#if RTC_ENABLE_WEBSOCKET
std::unordered_map<int, shared_ptr<WebSocket>> webSocketMap;
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
#if RTC_ENABLE_MEDIA
	rtcpSrReporterMap.erase(tr);
	rtcpChainableHandlerMap.erase(tr);
	rtpConfigMap.erase(tr);
#endif
	userPointerMap.erase(tr);
}

#if RTC_ENABLE_MEDIA

shared_ptr<RtcpSrReporter> getRtcpSrReporter(int id) {
	std::lock_guard lock(mutex);
	if (auto it = rtcpSrReporterMap.find(id); it != rtcpSrReporterMap.end()) {
		return it->second;
	} else {
		throw std::invalid_argument("RtcpSRReporter ID does not exist");
	}
}

void emplaceRtcpSrReporter(shared_ptr<RtcpSrReporter> ptr, int tr) {
	std::lock_guard lock(mutex);
	rtcpSrReporterMap.emplace(std::make_pair(tr, ptr));
}

shared_ptr<MediaChainableHandler> getMediaChainableHandler(int id) {
	std::lock_guard lock(mutex);
	if (auto it = rtcpChainableHandlerMap.find(id); it != rtcpChainableHandlerMap.end()) {
		return it->second;
	} else {
		throw std::invalid_argument("RtcpChainableHandler ID does not exist");
	}
}

void emplaceMediaChainableHandler(shared_ptr<MediaChainableHandler> ptr, int tr) {
	std::lock_guard lock(mutex);
	rtcpChainableHandlerMap.emplace(std::make_pair(tr, ptr));
}

shared_ptr<RtpPacketizationConfig> getRTPConfig(int id) {
	std::lock_guard lock(mutex);
	if (auto it = rtpConfigMap.find(id); it != rtpConfigMap.end()) {
		return it->second;
	} else {
		throw std::invalid_argument("RTPConfiguration ID does not exist");
	}
}

void emplaceRTPConfig(shared_ptr<RtpPacketizationConfig> ptr, int tr) {
	std::lock_guard lock(mutex);
	rtpConfigMap.emplace(std::make_pair(tr, ptr));
}

Description::Direction rtcDirectionToDirection(rtcDirection direction) {
	switch (direction) {
	case RTC_DIRECTION_SENDONLY:
		return Description::Direction::SendOnly;
	case RTC_DIRECTION_RECVONLY:
		return Description::Direction::RecvOnly;
	case RTC_DIRECTION_SENDRECV:
		return Description::Direction::SendRecv;
	case RTC_DIRECTION_INACTIVE:
		return Description::Direction::Inactive;
	default:
		return Description::Direction::Unknown;
	}
}

shared_ptr<RtpPacketizationConfig>
getNewRtpPacketizationConfig(uint32_t ssrc, const char *cname, uint8_t payloadType,
                             uint32_t clockRate, uint16_t sequenceNumber, uint32_t timestamp) {
	if (!cname) {
		throw std::invalid_argument("Unexpected null pointer for cname");
	}

	return std::make_shared<RtpPacketizationConfig>(ssrc, cname, payloadType, clockRate,
	                                                sequenceNumber, timestamp);
}

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

int copyAndReturn(string s, char *buffer, int size) {
	if (!buffer)
		return int(s.size() + 1);

	if (size < int(s.size()))
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
	buffer[b.size()] = '\0';
	return int(b.size());
}

class plogAppender : public plog::IAppender {
public:
	plogAppender(rtcLogCallbackFunc cb = nullptr) { setCallback(cb); }

	plogAppender(plogAppender &&appender) : callback(nullptr) {
		std::lock_guard lock(appender.callbackMutex);
		std::swap(appender.callback, callback);
	}

	void setCallback(rtcLogCallbackFunc cb) {
		std::lock_guard lock(callbackMutex);
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
		std::lock_guard lock(callbackMutex);
		if (callback)
			callback(static_cast<rtcLogLevel>(record.getSeverity()), str.c_str());
		else
			std::cout << plog::severityToString(severity) << " " << str << std::endl;
	}

private:
	rtcLogCallbackFunc callback;
	std::mutex callbackMutex;
};

} // namespace

void rtcInitLogger(rtcLogLevel level, rtcLogCallbackFunc cb) {
	static optional<plogAppender> appender;
	const auto severity = static_cast<plog::Severity>(level);
	std::lock_guard lock(mutex);
	if (appender) {
		appender->setCallback(cb);
		InitLogger(severity, nullptr); // change the severity
	} else if (cb) {
		appender.emplace(plogAppender(cb));
		InitLogger(severity, &appender.value());
	} else {
		InitLogger(severity, nullptr); // log to stdout
	}
}

void rtcSetUserPointer(int i, void *ptr) { setUserPointer(i, ptr); }

void *rtcGetUserPointer(int i) { return getUserPointer(i).value_or(nullptr); }

int rtcCreatePeerConnection(const rtcConfiguration *config) {
	return wrap([config] {
		Configuration c;
		for (int i = 0; i < config->iceServersCount; ++i)
			c.iceServers.emplace_back(string(config->iceServers[i]));

		c.enableIceTcp = config->enableIceTcp;
		c.disableAutoNegotiation = config->disableAutoNegotiation;

		if (config->portRangeBegin > 0 || config->portRangeEnd > 0) {
			c.portRangeBegin = config->portRangeBegin;
			c.portRangeEnd = config->portRangeEnd;
		}

		if(config->mtu > 0)
			c.mtu = size_t(config->mtu);

		return emplacePeerConnection(std::make_shared<PeerConnection>(c));
	});
}

int rtcDeletePeerConnection(int pc) {
	return wrap([pc] {
		auto peerConnection = getPeerConnection(pc);
		peerConnection->onDataChannel(nullptr);
		peerConnection->onTrack(nullptr);
		peerConnection->onLocalDescription(nullptr);
		peerConnection->onLocalCandidate(nullptr);
		peerConnection->onStateChange(nullptr);
		peerConnection->onGatheringStateChange(nullptr);

		erasePeerConnection(pc);
		return RTC_ERR_SUCCESS;
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
				if (reliability->maxPacketLifeTime > 0) {
					dci.reliability.type = Reliability::Type::Timed;
					dci.reliability.rexmit = milliseconds(reliability->maxPacketLifeTime);
				} else {
					dci.reliability.type = Reliability::Type::Rexmit;
					dci.reliability.rexmit = reliability->maxRetransmits;
				}
			} else {
				dci.reliability.type = Reliability::Type::Reliable;
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
		dataChannel->onOpen(nullptr);
		dataChannel->onClosed(nullptr);
		dataChannel->onError(nullptr);
		dataChannel->onMessage(nullptr);
		dataChannel->onBufferedAmountLow(nullptr);
		dataChannel->onAvailable(nullptr);

		eraseDataChannel(dc);
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

int rtcAddTrackEx(int pc, rtcCodec codec, int payloadType, uint32_t ssrc, const char *_mid,
                  rtcDirection _direction, const char *_name, const char *_msid,
                  const char *_trackID) {
	return wrap([&] {
		auto peerConnection = getPeerConnection(pc);

		auto direction = rtcDirectionToDirection(_direction);

		string mid = "video";
		switch (codec) {
		case RTC_CODEC_H264:
		case RTC_CODEC_VP8:
		case RTC_CODEC_VP9:
			mid = "video";
			break;
		case RTC_CODEC_OPUS:
			mid = "audio";
			break;
		}

		if (_mid) {
			mid = string(_mid);
		}

		optional<Description::Media> optDescription = nullopt;

		switch (codec) {
		case RTC_CODEC_H264:
		case RTC_CODEC_VP8:
		case RTC_CODEC_VP9: {
			auto desc = Description::Video(mid, direction);
			switch (codec) {
			case RTC_CODEC_H264:
				desc.addH264Codec(payloadType);
				break;
			case RTC_CODEC_VP8:
				desc.addVP8Codec(payloadType);
				break;
			case RTC_CODEC_VP9:
				desc.addVP8Codec(payloadType);
				break;
			default:
				break;
			}
			optDescription = desc;
			break;
		}
		case RTC_CODEC_OPUS: {
			auto desc = Description::Audio(mid, direction);
			switch (codec) {
			case RTC_CODEC_OPUS:
				desc.addOpusCodec(payloadType);
				break;
			default:
				break;
			}
			optDescription = desc;
			break;
		}
		default:
			break;
		}

		if (!optDescription.has_value()) {
			throw std::invalid_argument("Unexpected codec");
		} else {
			auto description = optDescription.value();
			setSSRC(&description, ssrc, _name, _msid, _trackID);

			int tr = emplaceTrack(peerConnection->addTrack(std::move(description)));
			if (auto ptr = getUserPointer(pc)) {
				rtcSetUserPointer(tr, *ptr);
			}
			return tr;
		}
	});
}

int rtcSetH264PacketizationHandler(int tr, uint32_t ssrc, const char *cname, uint8_t payloadType,
                                   uint32_t clockRate, uint16_t maxFragmentSize,
                                   uint16_t sequenceNumber, uint32_t timestamp) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = getNewRtpPacketizationConfig(ssrc, cname, payloadType, clockRate,
		                                              sequenceNumber, timestamp);
		// create packetizer
		auto packetizer = std::make_shared<H264RtpPacketizer>(rtpConfig, maxFragmentSize);
		// create H264 handler
		auto h264Handler = std::make_shared<H264PacketizationHandler>(packetizer);
		emplaceMediaChainableHandler(h264Handler, tr);
		emplaceRTPConfig(rtpConfig, tr);
		// set handler
		track->setRtcpHandler(h264Handler);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetOpusPacketizationHandler(int tr, uint32_t ssrc, const char *cname, uint8_t payloadType,
                                   uint32_t clockRate, uint16_t sequenceNumber,
                                   uint32_t timestamp) {
	return wrap([&] {
		auto track = getTrack(tr);
		// create RTP configuration
		auto rtpConfig = getNewRtpPacketizationConfig(ssrc, cname, payloadType, clockRate,
		                                              sequenceNumber, timestamp);
		// create packetizer
		auto packetizer = std::make_shared<OpusRtpPacketizer>(rtpConfig);
		// create Opus handler
		auto opusHandler = std::make_shared<OpusPacketizationHandler>(packetizer);
		emplaceMediaChainableHandler(opusHandler, tr);
		emplaceRTPConfig(rtpConfig, tr);
		// set handler
		track->setRtcpHandler(opusHandler);
		return RTC_ERR_SUCCESS;
	});
}

int rtcChainRtcpSrReporter(int tr) {
	return wrap([tr] {
		auto config = getRTPConfig(tr);
		auto reporter = std::make_shared<RtcpSrReporter>(config);
		emplaceRtcpSrReporter(reporter, tr);
		auto chainableHandler = getMediaChainableHandler(tr);
		chainableHandler->addToChain(reporter);
		return RTC_ERR_SUCCESS;
	});
}

int rtcChainRtcpNackResponder(int tr, unsigned maxStoredPacketsCount) {
	return wrap([tr, maxStoredPacketsCount] {
		auto responder = std::make_shared<RtcpNackResponder>(maxStoredPacketsCount);
		auto chainableHandler = getMediaChainableHandler(tr);
		chainableHandler->addToChain(responder);
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetRtpConfigurationStartTime(int id, double startTime_s, bool timeIntervalSince1970,
                                    uint32_t timestamp) {
	return wrap([&] {
		auto config = getRTPConfig(id);
		auto epoch = RtpPacketizationConfig::EpochStart::T1900;
		if (timeIntervalSince1970) {
			epoch = RtpPacketizationConfig::EpochStart::T1970;
		}
		config->setStartTime(startTime_s, epoch, timestamp);
		return RTC_ERR_SUCCESS;
	});
}

int rtcStartRtcpSenderReporterRecording(int id) {
	return wrap([id] {
		auto sender = getRtcpSrReporter(id);
		sender->startRecording();
		return RTC_ERR_SUCCESS;
	});
}

int rtcTransformSecondsToTimestamp(int id, double seconds, uint32_t *timestamp) {
	return wrap([&] {
		auto config = getRTPConfig(id);
		*timestamp = config->secondsToTimestamp(seconds);
		return RTC_ERR_SUCCESS;
	});
}

int rtcTransformTimestampToSeconds(int id, uint32_t timestamp, double *seconds) {
	return wrap([&] {
		auto config = getRTPConfig(id);
		*seconds = config->timestampToSeconds(timestamp);
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetCurrentTrackTimestamp(int id, uint32_t *timestamp) {
	return wrap([&] {
		auto config = getRTPConfig(id);
		*timestamp = config->timestamp;
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetTrackStartTimestamp(int id, uint32_t *timestamp) {
	return wrap([&] {
		auto config = getRTPConfig(id);
		*timestamp = config->startTimestamp;
		return RTC_ERR_SUCCESS;
	});
}

int rtcSetTrackRTPTimestamp(int id, uint32_t timestamp) {
	return wrap([&] {
		auto config = getRTPConfig(id);
		config->timestamp = timestamp;
		return RTC_ERR_SUCCESS;
	});
}

int rtcGetPreviousTrackSenderReportTimestamp(int id, uint32_t *timestamp) {
	return wrap([&] {
		auto sender = getRtcpSrReporter(id);
		*timestamp = sender->previousReportedTimestamp;
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

#endif // RTC_ENABLE_MEDIA

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

int rtcDeleteTrack(int tr) {
	return wrap([&] {
		auto track = getTrack(tr);
		track->onOpen(nullptr);
		track->onClosed(nullptr);
		track->onError(nullptr);
		track->onMessage(nullptr);
		track->onBufferedAmountLow(nullptr);
		track->onAvailable(nullptr);

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

#if RTC_ENABLE_WEBSOCKET
int rtcCreateWebSocket(const char *url) {
	return wrap([&] {
		auto ws = std::make_shared<WebSocket>();
		ws->open(url);
		return emplaceWebSocket(ws);
	});
}

int rtcCreateWebSocketEx(const char *url, const rtcWsConfiguration *config) {
	return wrap([&] {
		WebSocket::Configuration c;
		c.disableTlsVerification = config->disableTlsVerification;
		auto ws = std::make_shared<WebSocket>(c);
		ws->open(url);
		return emplaceWebSocket(ws);
	});
}

int rtcDeleteWebsocket(int ws) {
	return wrap([&] {
		auto webSocket = getWebSocket(ws);
		webSocket->onOpen(nullptr);
		webSocket->onClosed(nullptr);
		webSocket->onError(nullptr);
		webSocket->onMessage(nullptr);
		webSocket->onBufferedAmountLow(nullptr);
		webSocket->onAvailable(nullptr);

		eraseWebSocket(ws);
		return RTC_ERR_SUCCESS;
	});
}
#endif

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

int rtcGetDataChannelStream(int dc) {
	return wrap([dc] {
		auto dataChannel = getDataChannel(dc);
		return int(dataChannel->id());
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
		if (dcr.type == Reliability::Type::Timed) {
			reliability->unreliable = true;
			reliability->maxPacketLifeTime = int(std::get<milliseconds>(dcr.rexmit).count());
		} else if (dcr.type == Reliability::Type::Rexmit) {
			reliability->unreliable = true;
			reliability->maxRetransmits = std::get<int>(dcr.rexmit);
		} else {
			reliability->unreliable = false;
		}
		return RTC_ERR_SUCCESS;
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
				        channel->receive(); // discard
				        *size = ret;
				        return RTC_ERR_SUCCESS;
			        } else {
				        *size = int(b.size());
				        return ret;
			        }
		        },
		        [&](string s) {
			        int ret = copyAndReturn(std::move(s), buffer, *size);
			        if (ret >= 0) {
				        channel->receive(); // discard
				        *size = -ret;
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

void rtcPreload() { rtc::Preload(); }
void rtcCleanup() { rtc::Cleanup(); }
