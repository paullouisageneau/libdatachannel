/**
 * Copyright (c) 2025 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTCP_PAUSE_RESUME_REQUESTER_H
#define RTC_RTCP_PAUSE_RESUME_REQUESTER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "rtp.hpp"
#include "utils.hpp"

#include <functional>
#include <mutex>
#include <unordered_map>

namespace rtc {

/// Receiver-side MediaHandler for RFC 7728 pause/resume.
/// Sends PAUSE/RESUME requests and handles PAUSED/REFUSED responses.
class RTC_CPP_EXPORT RtcpPauseResumeRequester final : public MediaHandler {
public:
	enum class StreamState : uint8_t {
		Playing,          // Stream is active (or unknown)
		PauseRequested,   // PAUSE sent, waiting for PAUSED/REFUSED
		Paused,           // PAUSED received, stream is paused
		ResumeRequested,  // RESUME sent, waiting for stream to resume
	};

	/// Construct a receiver-side pause/resume requester.
	/// @param ssrc Our receiver SSRC (used as packet sender SSRC in RTCP)
	RtcpPauseResumeRequester(SSRC ssrc);

	void incoming(message_vector &messages, const message_callback &send) override;

	/// Chain-compatible pause/resume (called via MediaHandler::pauseStream)
	bool pauseStream(uint32_t ssrc, const message_callback &send) override;
	bool resumeStream(uint32_t ssrc, const message_callback &send) override;
	void onStreamPaused(std::function<void(uint32_t, uint16_t, uint32_t)> callback) override;
	void onStreamRefused(std::function<void(uint32_t, uint16_t)> callback) override;

	/// Send a PAUSE request for the given target SSRC.
	void requestPause(SSRC targetSsrc, const message_callback &send);

	/// Send a RESUME request for the given target SSRC.
	void requestResume(SSRC targetSsrc, const message_callback &send);

	/// Get the current stream state for a target SSRC.
	[[nodiscard]] StreamState streamState(SSRC targetSsrc) const;

	/// Get the currently tracked PauseID for a target SSRC.
	[[nodiscard]] uint16_t trackedPauseId(SSRC targetSsrc) const;

	/// Register callback for when a stream resumes.
	/// Parameter: target SSRC.
	void onStreamResumed(std::function<void(SSRC)> callback);

private:
	struct StreamInfo {
		StreamState state = StreamState::Playing;
		uint16_t pauseId = 0;
	};

	StreamInfo &getOrCreateStream(SSRC ssrc);
	const StreamInfo *getStream(SSRC ssrc) const;

	SSRC mSsrc;
	std::unordered_map<SSRC, StreamInfo> mStreams;
	std::function<void(SSRC, uint16_t, uint32_t)> mOnPaused;
	std::function<void(SSRC)> mOnResumed;
	std::function<void(SSRC, uint16_t)> mOnRefused;
	mutable std::mutex mMutex;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_RTCP_PAUSE_RESUME_REQUESTER_H
