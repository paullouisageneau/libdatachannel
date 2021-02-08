/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RTC_RTCP_NACK_RESPONDER_H
#define RTC_RTCP_NACK_RESPONDER_H

#if RTC_ENABLE_MEDIA

#include "mediahandlerelement.hpp"

#include <unordered_map>
#include <queue>

namespace rtc {

class RTC_CPP_EXPORT RtcpNackResponder: public MediaHandlerElement {

	/// Packet storage
	class RTC_CPP_EXPORT Storage {
		
		/// Packet storage element
		struct RTC_CPP_EXPORT Element {
			Element(binary_ptr packet, uint16_t sequenceNumber, std::shared_ptr<Element> next = nullptr);
			const binary_ptr packet;
			const uint16_t sequenceNumber;
			/// Pointer to newer element
			std::shared_ptr<Element> next = nullptr;
		};

	private:
		/// Oldest packet in storage
		std::shared_ptr<Element> oldest = nullptr;
		/// Newest packet in storage
		std::shared_ptr<Element> newest = nullptr;

		/// Inner storage
		std::unordered_map<uint16_t, std::shared_ptr<Element>> storage{};

		/// Maximum storage size
		const unsigned maximumSize;

		/// Returnst current size
		unsigned size();

	public:
		static const unsigned defaultMaximumSize = 512;

		Storage(unsigned _maximumSize);

		/// Returns packet with given sequence number
		std::optional<binary_ptr> get(uint16_t sequenceNumber);

		/// Stores packet
		/// @param packet Packet
		void store(binary_ptr packet);
	};

	const std::shared_ptr<Storage> storage;
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
	ChainedOutgoingProduct processOutgoingBinaryMessage(ChainedMessagesProduct messages, message_ptr control) override;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_NACK_RESPONDER_H */
