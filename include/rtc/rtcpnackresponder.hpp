/**
 * Copyright (c) 2020 Filip Klembara (in2core)
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

#ifndef RTC_RTCP_NACK_RESPONDER_H
#define RTC_RTCP_NACK_RESPONDER_H

#if RTC_ENABLE_MEDIA

#include "mediahandlerelement.hpp"

#include <queue>
#include <unordered_map>

namespace rtc {

class RTC_CPP_EXPORT RtcpNackResponder final : public MediaHandlerElement {

	/// Packet storage
	class RTC_CPP_EXPORT Storage {

		/// Packet storage element
		struct RTC_CPP_EXPORT Element {
			Element(binary_ptr packet, uint16_t sequenceNumber, shared_ptr<Element> next = nullptr);
			const binary_ptr packet;
			const uint16_t sequenceNumber;
			/// Pointer to newer element
			shared_ptr<Element> next = nullptr;
		};

	private:
		/// Oldest packet in storage
		shared_ptr<Element> oldest = nullptr;
		/// Newest packet in storage
		shared_ptr<Element> newest = nullptr;

		/// Inner storage
		std::unordered_map<uint16_t, shared_ptr<Element>> storage{};

		/// Maximum storage size
		const unsigned maximumSize;

		/// Returnst current size
		unsigned size();

	public:
		static const unsigned defaultMaximumSize = 512;

		Storage(unsigned _maximumSize);

		/// Returns packet with given sequence number
		optional<binary_ptr> get(uint16_t sequenceNumber);

		/// Stores packet
		/// @param packet Packet
		void store(binary_ptr packet);
	};

	const shared_ptr<Storage> storage;
	std::mutex reportMutex;

public:
	RtcpNackResponder(unsigned maxStoredPacketCount = Storage::defaultMaximumSize);

	/// Checks for RTCP NACK and handles it,
	/// @param message RTCP message
	/// @returns unchanged RTCP message and requested RTP packets
	ChainedIncomingControlProduct processIncomingControlMessage(message_ptr message) override;

	/// Stores RTP packets in internal storage
	/// @param messages RTP packets
	/// @param control RTCP
	/// @returns Unchanged RTP and RTCP
	ChainedOutgoingProduct processOutgoingBinaryMessage(ChainedMessagesProduct messages,
	                                                    message_ptr control) override;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_NACK_RESPONDER_H */
