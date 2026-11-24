/**
 * Copyright (c) 2025 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcppauseresumerequester.hpp"

#include <cstring>

namespace rtc {

RtcpPauseResumeRequester::RtcpPauseResumeRequester(SSRC ssrc) : mSsrc(ssrc) {}

void RtcpPauseResumeRequester::incoming(message_vector &messages, const message_callback &send) {
	(void)send;

	for (const auto &message : messages) {
		if (message->type == Message::Control) {
			auto *pr = RtcpPauseResume::Parse(message->data(), message->size());
			if (!pr)
				continue;

			// Iterate over FCI entries
			size_t packetSize = pr->getSize();
			size_t fciOffset = 0;
			const auto *fciBase = pr->getFci(0);
			const auto *raw = reinterpret_cast<const uint8_t *>(fciBase);
			size_t fciAreaSize = packetSize - sizeof(RtcpFbHeader);

			while (fciOffset + RtcpPauseResumeFci::BaseSize <= fciAreaSize) {
				const auto *fci = reinterpret_cast<const RtcpPauseResumeFci *>(raw + fciOffset);
				SSRC targetSsrc = fci->targetSsrc();
				auto type = fci->type();
				uint16_t pauseId = fci->pauseId();

				std::lock_guard lock(mMutex);

				switch (type) {
				case RtcpPauseResumeType::Paused: {
					// Sender has paused this stream
					auto &info = getOrCreateStream(targetSsrc);
					info.state = StreamState::Paused;
					info.pauseId = pauseId;
					uint32_t extHighSeq = 0;
					if (fci->parameterLen() >= 1)
						extHighSeq = fci->extendedHighestSeqNo();
					if (mOnPaused)
						mOnPaused(targetSsrc, pauseId, extHighSeq);
					break;
				}
				case RtcpPauseResumeType::Refused: {
					// Sender refused our request
					auto &info = getOrCreateStream(targetSsrc);
					info.state = StreamState::Playing;
					info.pauseId = pauseId; // Update to correct PauseID
					if (mOnRefused)
						mOnRefused(targetSsrc, pauseId);
					break;
				}
				default:
					// PAUSE and RESUME are receiver->sender; ignore on receiver side
					break;
				}

				fciOffset += fci->getSize();
			}
		} else if (message->type == Message::Binary) {
			// RTP data packet — detect stream resumption.
			// If we were in Paused or ResumeRequested state and RTP arrives,
			// the sender has resumed. Transition to Playing and fire callback.
			if (message->size() >= 12) { // minimum RTP header size
				auto *rtpData = reinterpret_cast<const uint8_t *>(message->data());
				// Extract SSRC from RTP header (bytes 8-11)
				uint32_t ssrc = (uint32_t(rtpData[8]) << 24) | (uint32_t(rtpData[9]) << 16) |
				                (uint32_t(rtpData[10]) << 8) | uint32_t(rtpData[11]);

				std::lock_guard lock(mMutex);
				auto it = mStreams.find(ssrc);
				if (it != mStreams.end() &&
				    (it->second.state == StreamState::Paused ||
				     it->second.state == StreamState::ResumeRequested)) {
					it->second.state = StreamState::Playing;
					// RFC 7728 Section 6.1: PauseID increments when entering Playing state
					it->second.pauseId = (it->second.pauseId + 1) & 0xFFFF;
					if (mOnResumed)
						mOnResumed(ssrc);
				}
			}
		}
	}
}

void RtcpPauseResumeRequester::requestPause(SSRC targetSsrc, const message_callback &send) {
	std::lock_guard lock(mMutex);
	auto &info = getOrCreateStream(targetSsrc);
	auto pkt = RtcpPauseResume::BuildPause(mSsrc, targetSsrc, info.pauseId);
	auto msg = std::make_shared<Message>(std::move(pkt), Message::Control);
	info.state = StreamState::PauseRequested;
	send(msg);
}

void RtcpPauseResumeRequester::requestResume(SSRC targetSsrc, const message_callback &send) {
	std::lock_guard lock(mMutex);
	auto &info = getOrCreateStream(targetSsrc);
	auto pkt = RtcpPauseResume::BuildResume(mSsrc, targetSsrc, info.pauseId);
	auto msg = std::make_shared<Message>(std::move(pkt), Message::Control);
	info.state = StreamState::ResumeRequested;
	send(msg);
}

bool RtcpPauseResumeRequester::pauseStream(uint32_t ssrc, const message_callback &send) {
	requestPause(ssrc, send);
	return true;
}

bool RtcpPauseResumeRequester::resumeStream(uint32_t ssrc, const message_callback &send) {
	requestResume(ssrc, send);
	return true;
}

RtcpPauseResumeRequester::StreamState RtcpPauseResumeRequester::streamState(SSRC targetSsrc) const {
	std::lock_guard lock(mMutex);
	auto *info = getStream(targetSsrc);
	return info ? info->state : StreamState::Playing;
}

uint16_t RtcpPauseResumeRequester::trackedPauseId(SSRC targetSsrc) const {
	std::lock_guard lock(mMutex);
	auto *info = getStream(targetSsrc);
	return info ? info->pauseId : 0;
}

void RtcpPauseResumeRequester::onStreamPaused(std::function<void(SSRC, uint16_t, uint32_t)> callback) {
	std::lock_guard lock(mMutex);
	mOnPaused = std::move(callback);
}

void RtcpPauseResumeRequester::onStreamResumed(std::function<void(SSRC)> callback) {
	std::lock_guard lock(mMutex);
	mOnResumed = std::move(callback);
}

void RtcpPauseResumeRequester::onStreamRefused(std::function<void(SSRC, uint16_t)> callback) {
	std::lock_guard lock(mMutex);
	mOnRefused = std::move(callback);
}

RtcpPauseResumeRequester::StreamInfo &RtcpPauseResumeRequester::getOrCreateStream(SSRC ssrc) {
	return mStreams[ssrc];
}

const RtcpPauseResumeRequester::StreamInfo *RtcpPauseResumeRequester::getStream(SSRC ssrc) const {
	auto it = mStreams.find(ssrc);
	return it != mStreams.end() ? &it->second : nullptr;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
