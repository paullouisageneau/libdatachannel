/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpnackresponder.hpp"

#include "impl/internals.hpp"

#include <cassert>

namespace rtc {

RtcpNackResponder::Storage::Element::Element(binary_ptr packet, uint16_t sequenceNumber,
                                             shared_ptr<Element> next)
    : packet(packet), sequenceNumber(sequenceNumber), next(next) {}

unsigned RtcpNackResponder::Storage::size() { return unsigned(storage.size()); }

RtcpNackResponder::Storage::Storage(unsigned _maximumSize) : maximumSize(_maximumSize) {
	assert(maximumSize > 0);
	storage.reserve(maximumSize);
}

optional<binary_ptr> RtcpNackResponder::Storage::get(uint16_t sequenceNumber) {
	auto position = storage.find(sequenceNumber);
	return position != storage.end() ? std::make_optional(storage.at(sequenceNumber)->packet)
	                                 : nullopt;
}

void RtcpNackResponder::Storage::store(binary_ptr packet) {
	if (!packet || packet->size() < 12) {
		return;
	}
	auto rtp = reinterpret_cast<RtpHeader *>(packet->data());
	auto sequenceNumber = rtp->seqNumber();

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

	if (size() > maximumSize) {
		assert(oldest);
		if (oldest) {
			storage.erase(oldest->sequenceNumber);
			oldest = oldest->next;
		}
	}
}

RtcpNackResponder::RtcpNackResponder(unsigned maxStoredPacketCount)
    : MediaHandlerElement(), storage(std::make_shared<Storage>(maxStoredPacketCount)) {}

ChainedIncomingControlProduct
RtcpNackResponder::processIncomingControlMessage(message_ptr message) {
	optional<ChainedOutgoingProduct> optPackets = ChainedOutgoingProduct(nullptr);
	auto packets = make_chained_messages_product();

	size_t p = 0;
	while (p < message->size()) {
		auto nack = reinterpret_cast<RtcpNack *>(message->data() + p);
		p += nack->header.header.lengthInBytes();
		// check if rtcp is nack
		if (nack->header.header.payloadType() != 205 || nack->header.header.reportCount() != 1) {
			continue;
		}

		auto fieldsCount = nack->getSeqNoCount();

		std::vector<uint16_t> missingSequenceNumbers{};
		for (unsigned int i = 0; i < fieldsCount; i++) {
			auto field = nack->parts[i];
			auto newMissingSeqenceNumbers = field.getSequenceNumbers();
			missingSequenceNumbers.insert(missingSequenceNumbers.end(),
			                              newMissingSeqenceNumbers.begin(),
			                              newMissingSeqenceNumbers.end());
		}
		packets->reserve(packets->size() + missingSequenceNumbers.size());
		for (auto sequenceNumber : missingSequenceNumbers) {
			auto optPacket = storage->get(sequenceNumber);
			if (optPacket.has_value()) {
				auto packet = optPacket.value();
				packets->push_back(packet);
			}
		}
	}

	if (!packets->empty()) {
		return {message, ChainedOutgoingProduct(packets)};
	} else {
		return {message, nullopt};
	}
}

ChainedOutgoingProduct
RtcpNackResponder::processOutgoingBinaryMessage(ChainedMessagesProduct messages,
                                                message_ptr control) {
	for (auto message : *messages) {
		storage->store(message);
	}
	return {messages, control};
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
