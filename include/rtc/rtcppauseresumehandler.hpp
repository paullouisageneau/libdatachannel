/**
 * Copyright (c) 2025 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTCP_PAUSE_RESUME_HANDLER_H
#define RTC_RTCP_PAUSE_RESUME_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "rtp.hpp"
#include "utils.hpp"

#include <functional>
#include <mutex>

namespace rtc {

/// Sender-side MediaHandler implementing the RFC 7728 sender state machine.
/// Intercepts incoming RTCP PAUSE/RESUME messages targeting our SSRC,
/// manages the Playing/Pausing/Paused/LocalPaused state machine,
/// and gates outgoing RTP packets when paused.
class RTC_CPP_EXPORT RtcpPauseResumeHandler final : public MediaHandler {
public:
	enum class State : uint8_t {
		Playing,     // Normal sending
		Pausing,     // Received PAUSE, waiting hold-off (0 in point-to-point with nowait)
		Paused,      // Stream paused by remote request
		LocalPaused, // Stream paused by local decision (higher precedence)
	};

	/// Construct a sender-side pause/resume handler.
	/// @param ssrc Our sending SSRC that this handler manages
	/// @param nowait If true, hold-off period is 0 (point-to-point mode)
	RtcpPauseResumeHandler(SSRC ssrc, bool nowait = false);

	void incoming(message_vector &messages, const message_callback &send) override;
	void outgoing(message_vector &messages, const message_callback &send) override;

	/// Current state of the sender state machine
	[[nodiscard]] State state() const;

	/// Current PauseID for this stream
	[[nodiscard]] uint16_t currentPauseId() const;

	/// Locally pause the stream. Sends an unsolicited PAUSED indication so the remote
	/// learns the new state. The resulting LocalPaused state is remote-resumable —
	/// a remote RESUME at the matching pauseId will flip back to Playing via
	/// handleResume()'s LocalPaused branch. (This matches the RFC 8853 multi-tier
	/// deferred-pause design, where the sender pauses non-selected tiers at the wire
	/// level but the receiver picks the active tier via RESUME.)
	/// @param send Callback to send the PAUSED indication
	void localPause(const message_callback &send);

	/// Pause the stream due to SDP-signaled initial pause (RFC 8853 `~rid`).
	/// Like localPause(), the resulting LocalPaused state is remote-resumable.
	/// Unlike localPause(), this does not send a PAUSED indication (the remote already
	/// knows from the SDP it sent) and does not bump mPauseId.
	void setSdpPaused();

	/// Resume from local pause
	void localResume();

	/// Register a callback for state changes
	void onStateChanged(std::function<void(State)> callback);

private:
	void handlePause(uint16_t receivedPauseId, const message_callback &send);
	void handleResume(uint16_t receivedPauseId, const message_callback &send);
	void sendPausedIndication(const message_callback &send);
	void sendRefused(const message_callback &send, uint16_t pauseId);

	SSRC mSsrc;
	bool mNowait;
	State mState = State::Playing;
	uint16_t mPauseId = 0;
	uint32_t mHighestSeqNo = 0;
	bool mHasSeqNo = false;
	bool mSdpSignaledPause = false;
	std::function<void(State)> mStateCallback;
	mutable std::mutex mMutex;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_RTCP_PAUSE_RESUME_HANDLER_H
