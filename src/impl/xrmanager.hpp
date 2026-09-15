/**
 * Copyright (c) 2026 Kostya Vasilyev
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_XR_MANAGER_H
#define RTC_IMPL_XR_MANAGER_H

#include "common.hpp"
#include "message.hpp"

#if RTC_ENABLE_MEDIA

#include "rtc/rtp.hpp"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace rtc::impl {

// Handles the publisher side of RTCP XR RTT measurement: receiving Receiver Reference Time
// Report blocks (RRTR, RFC 3611 Section 4.4) and replying with Delay since Last Receiver Report
// blocks (DLRR, Section 4.5).
//
// This is deliberately not a MediaHandler. An RRTR's own SSRC (the sending XR packet's "SSRC of
// packet sender" field) is arbitrary and not guaranteed to match any track's media SSRC, so it
// cannot be captured via the per-track, SSRC-keyed MediaHandler chains that PeerConnection
// dispatches by. Capture (incoming) and reply (send) are therefore wired up independently by
// PeerConnection: incoming() is called unconditionally from PeerConnection::dispatchMedia(),
// before any track/SSRC routing; send() is called from PeerConnection::onTrackTransportSend(), which
// Track::transportSend() invokes on every outgoing packet, mirroring RtcpSrReporter's cadence.
class XrManager {
public:
	// Scans a possibly-compound RTCP message for RRTR blocks and records the latest one per
	// reporter SSRC. No-op for anything that isn't a Control message or doesn't contain an XR
	// (PT=207) sub-packet.
	void incoming(const message_ptr &message);

	// Sends a DLRR reply for any pending RRTR, using localSsrc as the sending identity. No-op if
	// there is nothing pending or if less than the flush interval has elapsed since the last
	// send.
	void send(SSRC localSsrc, const message_callback &sendCallback);

private:
	struct PendingReport {
		uint64_t ntpTimestamp = {};
		std::chrono::steady_clock::time_point receivedAt;
	};

	std::mutex mMutex;
	std::unordered_map<SSRC, PendingReport> mPending;
	std::chrono::steady_clock::time_point mLastFlush{};
};

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA

#endif
