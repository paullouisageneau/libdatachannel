/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTCP_NACK_RESPONDER_H
#define RTC_RTCP_NACK_RESPONDER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "description.hpp"
#include "rtp.hpp"

#include <mutex>
#include <queue>
#include <unordered_map>

namespace rtc {

class RTC_CPP_EXPORT RtcpNackResponder final : public MediaHandler {
public:
	static const size_t DefaultMaxSize = 512;

	RtcpNackResponder(size_t maxSize = DefaultMaxSize);

	void media(const Description::Media &desc) override;
	void incoming(message_vector &messages, const message_callback &send) override;
	void outgoing(message_vector &messages, const message_callback &send) override;

private:
	message_ptr wrapInRtx(const message_ptr &original);

	// RTX state populated by media() from SDP inspection
	optional<SSRC> mRtxSsrc;
	std::unordered_map<int, int> mRtxPayloadTypeMap; // original PT -> RTX PT
	uint16_t mRtxSequenceNumber = 0;
	bool mRtxEnabled = false;
	std::mutex mMutex;

	// Packet storage
	class RTC_CPP_EXPORT Storage {

		/// Packet storage element
		struct RTC_CPP_EXPORT Element {
			Element(message_ptr packet, uint16_t sequenceNumber, shared_ptr<Element> next = nullptr);
			const message_ptr packet;
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
		std::mutex mutex;

		/// Maximum storage size
		const size_t maxSize;

		/// Returns current size
		size_t size();

	public:
		Storage(size_t _maxSize);

		/// Returns packet with given sequence number
		message_ptr get(uint16_t sequenceNumber);

		/// Stores packet
		/// @param packet Packet
		void store(message_ptr packet);
	};

	const shared_ptr<Storage> mStorage;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTCP_NACK_RESPONDER_H */
