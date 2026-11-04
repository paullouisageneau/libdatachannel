/**
 * Copyright (c) 2025 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcppauseresumehandler.hpp"

#include "impl/internals.hpp"

#include <algorithm>
#include <cstring>

namespace rtc {

RtcpPauseResumeHandler::RtcpPauseResumeHandler(SSRC ssrc, bool nowait)
    : mSsrc(ssrc), mNowait(nowait) {}

void RtcpPauseResumeHandler::incoming(message_vector &messages, const message_callback &send) {
	for (const auto &message : messages) {
		if (message->type != Message::Control)
			continue;

		auto *pr = RtcpPauseResume::Parse(message->data(), message->size());
		if (!pr)
			continue;

		// Iterate over FCI entries in the packet.
		// Calculate number of FCI entries from packet size.
		size_t packetSize = pr->getSize();
		size_t fciOffset = 0;
		const auto *fciBase = pr->getFci(0);
		const auto *raw = reinterpret_cast<const uint8_t *>(fciBase);
		size_t fciAreaSize = packetSize - sizeof(RtcpFbHeader);

		while (fciOffset + RtcpPauseResumeFci::BaseSize <= fciAreaSize) {
			const auto *fci = reinterpret_cast<const RtcpPauseResumeFci *>(raw + fciOffset);

			// Only process FCI entries targeting our SSRC
			if (fci->targetSsrc() == mSsrc) {
				std::lock_guard lock(mMutex);
				auto type = fci->type();
				uint16_t receivedPauseId = fci->pauseId();

				switch (type) {
				case RtcpPauseResumeType::Pause:
					handlePause(receivedPauseId, send);
					break;
				case RtcpPauseResumeType::Resume:
					handleResume(receivedPauseId, send);
					break;
				case RtcpPauseResumeType::Paused:
				case RtcpPauseResumeType::Refused:
					// PAUSED and REFUSED are sender->receiver; ignore on sender side
					break;
				}
			}

			fciOffset += fci->getSize();
		}
	}
}

void RtcpPauseResumeHandler::outgoing(message_vector &messages, const message_callback &send) {
	(void)send;
	std::lock_guard lock(mMutex);

	// Track highest sequence number from outgoing RTP
	for (const auto &message : messages) {
		if (message->type == Message::Binary && message->size() >= sizeof(RtpHeader)) {
			auto *rtp = reinterpret_cast<const RtpHeader *>(message->data());
			uint16_t seq = rtp->seqNumber();
			if (!mHasSeqNo || seq > mHighestSeqNo || (mHighestSeqNo > 0xFF00 && seq < 0x0100)) {
				// Handle wrap-around: if current is near max and new is near 0,
				// treat new as higher
				mHighestSeqNo = seq;
				mHasSeqNo = true;
			}
		}
	}

	// Gate RTP when paused (either remotely or locally)
	if (mState == State::Paused || mState == State::LocalPaused) {
		// Remove all non-control messages (RTP data)
		messages.erase(
		    std::remove_if(messages.begin(), messages.end(),
		                   [](const message_ptr &msg) { return msg->type != Message::Control; }),
		    messages.end());
	}
}

RtcpPauseResumeHandler::State RtcpPauseResumeHandler::state() const {
	std::lock_guard lock(mMutex);
	return mState;
}

uint16_t RtcpPauseResumeHandler::currentPauseId() const {
	std::lock_guard lock(mMutex);
	return mPauseId;
}

void RtcpPauseResumeHandler::localPause(const message_callback &send) {
	std::lock_guard lock(mMutex);
	if (mState != State::Playing) {
		// Already gated in some pause sub-state (LocalPaused / Paused / Pausing).
		// Early-return so we do NOT:
		//   (a) re-emit an unsolicited PAUSED indication the remote already knows
		//       about — and which, when received against a Paused stream, looks like
		//       a noisy duplicate; and when received against a remote-resumed
		//       Playing stream, would mis-pause it.
		//   (b) escalate Paused → LocalPaused in a way the caller didn't ask for.
		//       A future caller that DOES want to escalate Paused → LocalPaused
		//       should use a dedicated API.
		return;
	}
	mState = State::LocalPaused;
	// Mark the LocalPaused state as remote-resumable. Without this flag set,
	// handleResume()'s LocalPaused branch would silently drop a remote RESUME at
	// the matching pauseId, locking the receiver out of any stream the sender has
	// locally paused. That's wrong for the RFC 8853 multi-tier use case where
	// receivers expect to be able to RESUME tiers post-connect; setting
	// mSdpSignaledPause=true makes handleResume's LocalPaused branch accept
	// the RESUME and flip back to Playing.
	//
	// The flag's name ("Sdp"-signaled) is a holdover from setSdpPaused() — semantically
	// it now means "remote-resumable LocalPaused" regardless of whether the pause
	// originated from an SDP `~rid` marker or from this localPause() call.
	mSdpSignaledPause = true;
	if (mStateCallback)
		mStateCallback(mState);
	sendPausedIndication(send);
}

void RtcpPauseResumeHandler::setSdpPaused() {
	std::lock_guard lock(mMutex);
	// Only honour SDP-signaled pause from the Playing state. If a remote PAUSE
	// has already moved us to Paused/Pausing, or another local pause is in
	// effect (LocalPaused), don't overwrite it — the remote-resume protocol
	// would otherwise flip back to Playing via the mSdpSignaledPause branch
	// even though the remote thought it was responsible for the prior pause.
	if (mState != State::Playing) {
		// Log so a future regression where this no-op silently drops SDP-marked
		// initial-paused intent is observable. Callers are expected to invoke this
		// while applying the SDP answer, before any RTCP traffic can move state off
		// Playing, so this branch should not normally fire — its firing signals a
		// contract change worth investigating.
		PLOG_DEBUG << "setSdpPaused: ignored, mState=" << static_cast<int>(mState);
		return;
	}
	// Note: we deliberately do NOT bump mPauseId here, and we do NOT send a PAUSED
	// indication. mPauseId stays at its current value (0 for a fresh handler), and
	// the remote that signaled the SDP pause is expected to send its first RESUME
	// with matching pauseId=0 — completing the SDP-signaled-pause/RESUME cycle
	// without a PAUSED indication round-trip. handleResume()'s LocalPaused branch
	// (gated on mSdpSignaledPause) accepts that transition.
	mState = State::LocalPaused;
	mSdpSignaledPause = true;
	if (mStateCallback)
		mStateCallback(mState);
}

void RtcpPauseResumeHandler::localResume() {
	std::lock_guard lock(mMutex);
	if (mState == State::LocalPaused) {
		mState = State::Playing;
		// Clear `mSdpSignaledPause` on transition out of LocalPaused. The flag is set on
		// every Playing → LocalPaused entry path (both `localPause()` and
		// `setSdpPaused()`), so clearing here is redundant with the sets in those two
		// methods but kept defensive: it gives the field a defined value during the
		// Playing window and means a future caller that ever introduces a non-Playing
		// entry path doesn't have to think about flag inheritance.
		mSdpSignaledPause = false;
		mPauseId = static_cast<uint16_t>((mPauseId + 1) & 0xFFFF);
		if (mStateCallback)
			mStateCallback(mState);
	}
}

void RtcpPauseResumeHandler::onStateChanged(std::function<void(State)> callback) {
	std::lock_guard lock(mMutex);
	mStateCallback = std::move(callback);
}

// Must be called with mMutex held
void RtcpPauseResumeHandler::handlePause(uint16_t receivedPauseId, const message_callback &send) {
	// Check PauseID matches
	if (receivedPauseId != mPauseId) {
		sendRefused(send, mPauseId);
		return;
	}

	switch (mState) {
	case State::Playing:
		if (mNowait) {
			// Point-to-point with nowait: go directly to Paused
			mState = State::Paused;
			if (mStateCallback)
				mStateCallback(mState);
			sendPausedIndication(send);
		} else {
			// Would enter Pausing state with hold-off timer
			// For now, treat as immediate (simplified for p2p use)
			mState = State::Paused;
			if (mStateCallback)
				mStateCallback(mState);
			sendPausedIndication(send);
		}
		break;
	case State::Pausing:
		// Already pausing, wait for hold-off
		break;
	case State::Paused:
		// Already paused, re-send PAUSED indication
		sendPausedIndication(send);
		break;
	case State::LocalPaused:
		// Local pause has higher precedence; ignore remote PAUSE
		break;
	}
}

// Must be called with mMutex held
void RtcpPauseResumeHandler::handleResume(uint16_t receivedPauseId, const message_callback &send) {
	// RFC 7728 Section 8.3: If PauseID is not current, send REFUSED —
	// EXCEPT if in Playing state and receives RESUME with a past PauseID,
	// in which case the RESUME SHALL be ignored.
	if (receivedPauseId != mPauseId) {
		if (mState == State::Playing) {
			// Silently ignore stale RESUME when already playing
			return;
		}
		sendRefused(send, mPauseId);
		return;
	}

	switch (mState) {
	case State::Playing:
		// Already playing, ignore RESUME with current PauseID (RFC 7728 Sec 8.3)
		break;
	case State::Pausing:
		// Cancel pending pause, return to Playing
		mState = State::Playing;
		// PauseID increments on entry to Playing
		mPauseId = static_cast<uint16_t>((mPauseId + 1) & 0xFFFF);
		if (mStateCallback)
			mStateCallback(mState);
		break;
	case State::Paused:
		// Resume from paused
		mState = State::Playing;
		mPauseId = static_cast<uint16_t>((mPauseId + 1) & 0xFFFF);
		if (mStateCallback)
			mStateCallback(mState);
		break;
	case State::LocalPaused:
		// RFC 8853: SDP-signaled pause accepts remote RESUME.
		// Regular local pause still has higher precedence.
		if (mSdpSignaledPause) {
			mSdpSignaledPause = false;
			mState = State::Playing;
			mPauseId = static_cast<uint16_t>((mPauseId + 1) & 0xFFFF);
			if (mStateCallback)
				mStateCallback(mState);
		}
		break;
	}
}

// Must be called with mMutex held
void RtcpPauseResumeHandler::sendPausedIndication(const message_callback &send) {
	uint32_t extHighSeq = mHasSeqNo ? mHighestSeqNo : 0;
	auto pkt = RtcpPauseResume::BuildPaused(mSsrc, mSsrc, mPauseId, extHighSeq);
	auto msg = std::make_shared<Message>(std::move(pkt), Message::Control);
	send(msg);
}

// Must be called with mMutex held
void RtcpPauseResumeHandler::sendRefused(const message_callback &send, uint16_t pauseId) {
	auto pkt = RtcpPauseResume::BuildRefused(mSsrc, mSsrc, pauseId);
	auto msg = std::make_shared<Message>(std::move(pkt), Message::Control);
	send(msg);
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
