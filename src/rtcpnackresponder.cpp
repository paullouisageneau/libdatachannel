/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpnackresponder.hpp"
#include "rtp.hpp"

#include "impl/internals.hpp"
#include "impl/utils.hpp"

#include <cassert>
#include <cstring>
#include <functional>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

namespace utils = impl::utils;

RtcpNackResponder::RtcpNackResponder(size_t maxSize)
    : mStorage(std::make_shared<Storage>(maxSize)) {
	auto uniform = std::bind(std::uniform_int_distribution<uint32_t>(), utils::random_engine());
	mRtxSequenceNumber = static_cast<uint16_t>(uniform());
}

void RtcpNackResponder::media(const Description::Media &desc) {
	bool newRtxEnabled = false;
	optional<SSRC> newRtxSsrc;
	std::unordered_map<int, int> newRtxPayloadTypeMap;

	if (desc.isRtxEnabled()) {
		auto pts = desc.payloadTypes();

		// Find RTX SSRC (shared across all codecs in the media section)
		auto ssrcs = desc.getSSRCs();
		for (auto ssrc : ssrcs) {
			auto rtxSsrc = desc.getRtxSsrcForSsrc(ssrc);
			if (rtxSsrc) {
				newRtxSsrc = *rtxSsrc;
				break;
			}
		}

		// Build mapping from each primary PT to its RTX PT
		for (int pt : pts) {
			auto rtxPt = desc.getRtxPayloadType(pt);
			if (rtxPt) {
				newRtxPayloadTypeMap[pt] = *rtxPt;
			}
		}

		newRtxEnabled = newRtxSsrc.has_value() && !newRtxPayloadTypeMap.empty();
	}

	std::lock_guard lock(mMutex);
	mRtxEnabled = newRtxEnabled;
	mRtxPayloadTypeMap = std::move(newRtxPayloadTypeMap);
	mRtxSsrc = newRtxSsrc;
}

void RtcpNackResponder::incoming(message_vector &messages, const message_callback &send) {
	bool rtxEnabled;
	{
		std::lock_guard lock(mMutex);
		rtxEnabled = mRtxEnabled;
	}

	for (const auto &message : messages) {
		if (message->type != Message::Control)
			continue;

		size_t p = 0;
		while (p + sizeof(RtcpNack) <= message->size()) {
			auto nack = reinterpret_cast<RtcpNack *>(message->data() + p);
			p += nack->header.header.lengthInBytes();
			if (p > message->size())
				break;

			// check if RTCP is NACK
			if (nack->header.header.payloadType() != 205 || nack->header.header.reportCount() != 1)
				continue;

			unsigned int fieldsCount = nack->getSeqNoCount();
			std::vector<uint16_t> missingSequenceNumbers;
			for (unsigned int i = 0; i < fieldsCount; i++) {
				auto field = nack->parts[i];
				auto newMissingSeqenceNumbers = field.getSequenceNumbers();
				missingSequenceNumbers.insert(missingSequenceNumbers.end(),
				                              newMissingSeqenceNumbers.begin(),
				                              newMissingSeqenceNumbers.end());
			}

			for (auto sequenceNumber : missingSequenceNumbers) {
				if (auto packet = mStorage->get(sequenceNumber)) {
					if (rtxEnabled) {
						// RTX sender mode: wrap in RTX before sending
						auto rtxPacket = wrapInRtx(packet);
						if (rtxPacket)
							send(rtxPacket);
					} else {
						// Plain retransmission
						send(packet);
					}
				}
			}
		}
	}
}

void RtcpNackResponder::outgoing(message_vector &messages,
                                 [[maybe_unused]] const message_callback &send) {
	for (const auto &message : messages)
		if (message->type != Message::Control)
			mStorage->store(message);
}

message_ptr RtcpNackResponder::wrapInRtx(const message_ptr &original) {
	if (!original || original->size() < sizeof(RtpHeader))
		return nullptr;

	auto origRtp = reinterpret_cast<const RtpHeader *>(original->data());
	int origPt = origRtp->payloadType();

	optional<SSRC> rtxSsrc;
	optional<int> rtxPayloadType;
	{
		std::lock_guard lock(mMutex);
		rtxSsrc = mRtxSsrc;
		auto it = mRtxPayloadTypeMap.find(origPt);
		if (it != mRtxPayloadTypeMap.end())
			rtxPayloadType = it->second;
	}

	if (!rtxSsrc || !rtxPayloadType)
		return nullptr;
	size_t headerSize =
	    static_cast<size_t>(origRtp->getBody() - reinterpret_cast<const char *>(origRtp));
	size_t origPayloadSize = original->size() - headerSize;

	// RTX packet = original header (modified) + 2-byte OSN + original payload
	size_t rtxSize = headerSize + sizeof(uint16_t) + origPayloadSize;
	auto rtxMessage = std::make_shared<Message>(rtxSize, Message::Binary);

	// Copy the original RTP header
	std::memcpy(rtxMessage->data(), original->data(), headerSize);

	auto rtxRtp = reinterpret_cast<RtpHeader *>(rtxMessage->data());

	// Modify header for RTX
	rtxRtp->setSsrc(*rtxSsrc);
	rtxRtp->setPayloadType(*rtxPayloadType);
	rtxRtp->setSeqNumber(mRtxSequenceNumber++);
	// Timestamp stays the same per RFC 4588 Section 4

	// Write the 2-byte Original Sequence Number
	uint16_t osn = htons(origRtp->seqNumber());
	std::memcpy(rtxMessage->data() + headerSize, &osn, sizeof(uint16_t));

	// Copy original payload
	if (origPayloadSize > 0)
		std::memcpy(rtxMessage->data() + headerSize + sizeof(uint16_t),
		            original->data() + headerSize, origPayloadSize);

	rtxMessage->stream = *rtxSsrc;
	return rtxMessage;
}

RtcpNackResponder::Storage::Element::Element(message_ptr packet, uint16_t sequenceNumber,
                                             shared_ptr<Element> next)
    : packet(packet), sequenceNumber(sequenceNumber), next(next) {}

size_t RtcpNackResponder::Storage::size() { return storage.size(); }

RtcpNackResponder::Storage::Storage(size_t _maxSize) : maxSize(_maxSize) {
	assert(maxSize > 0);
	storage.reserve(maxSize);
}

message_ptr RtcpNackResponder::Storage::get(uint16_t sequenceNumber) {
	std::lock_guard lock(mutex);
	auto position = storage.find(sequenceNumber);
	return position != storage.end() ? storage.at(sequenceNumber)->packet
	                                 : nullptr;
}

void RtcpNackResponder::Storage::store(message_ptr packet) {
	if (!packet || packet->size() < sizeof(RtpHeader))
		return;

	auto rtp = reinterpret_cast<RtpHeader *>(packet->data());
	auto sequenceNumber = rtp->seqNumber();

	std::lock_guard lock(mutex);
	assert((storage.empty() && !oldest && !newest) || (!storage.empty() && oldest && newest));

	if (size() == 0) {
		newest = std::make_shared<Element>(packet, sequenceNumber);
		oldest = newest;
	} else {
		auto current = std::make_shared<Element>(packet, sequenceNumber);
		newest->next = current;
		newest = current;
	}

	storage.emplace(sequenceNumber, newest);

	if (size() > maxSize) {
		assert(oldest);
		if (oldest) {
			storage.erase(oldest->sequenceNumber);
			oldest = oldest->next;
		}
	}
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
