/**
 * Copyright (c) 2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtpdepacketizer.hpp"
#include "rtp.hpp"
#include "sframe.hpp" // for SFrameUtility descriptor helpers

#include "impl/logcounter.hpp"
#include "impl/sframeutility.hpp"

namespace rtc {

RtpDepacketizer::RtpDepacketizer() : mClockRate(0) {}

RtpDepacketizer::RtpDepacketizer(uint32_t clockRate) : mClockRate(clockRate) {}

RtpDepacketizer::~RtpDepacketizer() {}

void RtpDepacketizer::incoming(message_vector &messages,
                               [[maybe_unused]] const message_callback &send) {
	message_vector result;
	for (auto &message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			continue;
		}

		if (message->size() < sizeof(RtpHeader)) {
			PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
			continue;
		}

		auto header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
		if (message->size() < header->getSize())
			continue; // truncated header

		auto totalHeaderSize = header->getSize() + header->getExtensionHeaderSize();
		if (message->size() < totalHeaderSize)
			continue; // truncated header

		result.push_back(make_message(message->begin() + totalHeaderSize, message->end(),
		                              createFrameInfo(header->timestamp(), header->payloadType())));
	}

	messages.swap(result);
}

shared_ptr<FrameInfo> RtpDepacketizer::createFrameInfo(uint32_t timestamp,
                                                       uint8_t payloadType) const {
	auto frameInfo = std::make_shared<FrameInfo>(timestamp);
	if (mClockRate > 0)
		frameInfo->timestampSeconds =
		    std::chrono::duration<double>(double(timestamp) / double(mClockRate));
	frameInfo->payloadType = payloadType;
	return frameInfo;
}

VideoRtpDepacketizer::VideoRtpDepacketizer() : RtpDepacketizer(ClockRate) {}

VideoRtpDepacketizer::~VideoRtpDepacketizer() {}

static impl::LogCounter
    COUNTER_SFRAME_INCOMPLETE(plog::warning,
                              "Number of incomplete SFrame frames dropped from the buffer");
static impl::LogCounter
    COUNTER_SFRAME_MALFORMED(plog::warning, "Number of malformed SFrame frames dropped");
static impl::LogCounter COUNTER_SFRAME_PACKETIZED_ORIGIN(
    plog::warning, "Number of SFrame video packets dropped for unsupported "
                   "per-packet SFrame (descriptor T=1)");

// Checks one delimited run and returns the total payload size, or nullopt if it is not a
// well-formed object. Kept separate from assembly so the completeness check can run without
// copying: it runs on every buffered group on every packet.
std::optional<size_t> VideoRtpDepacketizer::validateSFrameRun(const message_vector &run) {
	size_t total = 0;
	uint8_t pt = 0;
	const size_t count = run.size();
	for (size_t idx = 0; idx < count; ++idx) {
		const auto &pkt = run[idx];
		size_t hdrSize = 0, payloadEnd = 0;
		if (!impl::SFrameUtility::ParseSFramePacket(pkt, hdrSize, payloadEnd))
			return std::nullopt;

		const uint8_t descriptor = static_cast<uint8_t>(*(pkt->begin() + hdrSize));
		if (!impl::SFrameUtility::IsValidDescriptor(descriptor, idx == 0, idx + 1 == count))
			return std::nullopt;

		// draft-ietf-avtcore-rtp-sframe section 5.2: the payload type must be the same across
		// the run, or the object spans two encodings.
		auto header = reinterpret_cast<const RtpHeader *>(pkt->data());
		if (idx == 0)
			pt = header->payloadType();
		else if (header->payloadType() != pt)
			return std::nullopt;

		total += payloadEnd - (hdrSize + 1);
	}

	if (total == 0)
		return std::nullopt;

	return total;
}

// Concatenates one delimited run's payloads, descriptor and padding removed.
bool VideoRtpDepacketizer::assembleSFrameObject(const message_vector &run,
                                                message_vector &result) {
	auto size = validateSFrameRun(run);
	if (!size)
		return false;

	auto first = reinterpret_cast<const RtpHeader *>(run.front()->data());
	const uint32_t ts = first->timestamp();
	const uint8_t pt = first->payloadType();

	binary assembled;
	assembled.reserve(*size);
	for (const auto &pkt : run) {
		size_t hdrSize = 0, payloadEnd = 0;
		impl::SFrameUtility::ParseSFramePacket(pkt, hdrSize, payloadEnd); // checked above
		assembled.insert(assembled.end(), pkt->begin() + hdrSize + 1, pkt->begin() + payloadEnd);
	}

	result.push_back(make_message(std::move(assembled), createFrameInfo(ts, pt)));
	return true;
}

// draft-ietf-avtcore-rtp-sframe section 5.2: an object is the smallest run of consecutive
// packets from S to E, so one timestamp can hold several. Runs must also be contiguous in
// sequence number, or an object missing a middle packet would be spliced together.
// Returns false if any packet was not consumed by a well-formed run.
// True when every packet in the group belongs to a complete S...E run, so nothing further
// is expected for this timestamp.
bool VideoRtpDepacketizer::sframeGroupResolves(const message_buffer &packets) {
	message_vector discard;
	return assembleSFrameObjects(packets, discard, /*dryRun=*/true);
}

bool VideoRtpDepacketizer::assembleSFrameObjects(const message_buffer &packets,
                                                 message_vector &result, bool dryRun) {
	message_vector staged;
	bool allConsumed = true;
	message_vector run;
	uint16_t expectedSeq = 0;

	for (const auto &pkt : packets) {
		size_t hdrSize = 0, payloadEnd = 0;
		if (!impl::SFrameUtility::ParseSFramePacket(pkt, hdrSize, payloadEnd)) {
			allConsumed = false;
			run.clear();
			continue;
		}

		const uint8_t descriptor = static_cast<uint8_t>(*(pkt->begin() + hdrSize));
		auto header = reinterpret_cast<const RtpHeader *>(pkt->data());

		// T is the only thing separating this from a single-packet per-frame object, so
		// count it separately rather than as generic corruption.
		if (impl::SFrameUtility::IsPacketizedOrigin(descriptor)) {
			COUNTER_SFRAME_PACKETIZED_ORIGIN++;
			allConsumed = false;
			run.clear();
			continue;
		}

		if (descriptor & SFRAME_DESCRIPTOR_S) {
			// A new object starts here, so anything still open lost its E packet.
			if (!run.empty())
				allConsumed = false;
			run.clear();
		} else if (run.empty() || header->seqNumber() != expectedSeq) {
			// A continuation with no open run, or a gap in the middle of one.
			allConsumed = false;
			run.clear();
			continue;
		}

		run.push_back(pkt);
		expectedSeq = static_cast<uint16_t>(header->seqNumber() + 1);

		if (!(descriptor & SFRAME_DESCRIPTOR_E))
			continue;

		// The dry run only asks whether the group is complete, so it skips the copy.
		const bool ok = dryRun ? validateSFrameRun(run).has_value()
		                       : assembleSFrameObject(run, staged);
		if (!ok)
			allConsumed = false;
		run.clear();
	}

	if (!run.empty())
		allConsumed = false; // an object whose E packet never arrived

	if (dryRun)
		return allConsumed;

	if (!allConsumed)
		COUNTER_SFRAME_MALFORMED++;

	result.insert(result.end(), std::make_move_iterator(staged.begin()),
	              std::make_move_iterator(staged.end()));
	return allConsumed;
}

// A packet older than the last group delivered cannot be reassembled into it any more, and
// buffering it would re-create a bucket at the head of the map that never completes,
// blocking every later frame until eviction. Drop it instead. Wrap-safe, as for sequence
// numbers. Equal timestamps are not stale: several SFrame objects may share one.
bool VideoRtpDepacketizer::isStaleSFrameTimestamp(uint32_t timestamp) const {
	return mSFrameEmitted && int32_t(timestamp - mSFrameLastEmitted) < 0;
}

// Hands one buffered group over: whatever forms complete S...E runs is emitted, the rest is
// dropped as unrecoverable.
void VideoRtpDepacketizer::releaseSFrameGroup(size_t index, message_vector &result) {
	auto &group = mSFrames[index];
	const size_t before = result.size();
	assembleSFrameObjects(group.packets, result);

	// The watermark only moves for a timestamp something was actually delivered for. A group
	// that yielded nothing was never part of this stream, and letting it set the watermark
	// would make every genuine packet behind it look like a late straggler.
	if (result.size() > before) {
		mSFrameLastEmitted = group.timestamp;
		mSFrameEmitted = true;
	}

	for (const auto &pkt : group.packets)
		mSFrameBufferedBytes -= pkt->size();
	mSFrames.erase(mSFrames.begin() + index);
}

// Hand over every group up to and including `timestamp`, complete or not. Mirrors the
// codec path, which finishes a frame on the marker bit as well as on a timestamp change.
void VideoRtpDepacketizer::flushSFramesThrough(uint32_t timestamp, message_vector &result) {
	while (!mSFrames.empty()) {
		const bool last = mSFrames.front().timestamp == timestamp;
		releaseSFrameGroup(0, result);
		if (last)
			break;
	}
}

// Once a newer group has been buffered, every earlier one holds all the packets it is going
// to get, so hand them over even if one is incomplete. This bounds head-of-line blocking for
// a sender that leaves the marker clear; with the marker set, the flush above has already
// finished the group. The newest group stays pending: more may still arrive.
void VideoRtpDepacketizer::flushSFramesExceptNewest(message_vector &result) {
	while (mSFrames.size() > 1)
		releaseSFrameGroup(0, result);
}

void VideoRtpDepacketizer::bufferSFramePacket(message_ptr packet, uint32_t timestamp) {
	auto it = std::find_if(mSFrames.begin(), mSFrames.end(),
	                       [timestamp](const SFrameGroup &g) { return g.timestamp == timestamp; });
	if (it == mSFrames.end())
		it = mSFrames.insert(mSFrames.end(), SFrameGroup{timestamp, {}});

	// A duplicate sequence number is not stored, so only count what the set accepted:
	// otherwise the byte total drifts up permanently and the caps fire on an empty map.
	const size_t size = packet->size();
	if (it->packets.insert(std::move(packet)).second)
		mSFrameBufferedBytes += size;
}

// Emits, in arrival order, every leading group whose packets already form whole S...E runs.
// Completion is judged from the E bit rather than the RTP marker: the marker is
// profile-specific, while every SFrame packet carries a descriptor.
void VideoRtpDepacketizer::emitCompleteSFrames(message_vector &result) {
	while (!mSFrames.empty() && sframeGroupResolves(mSFrames.front().packets))
		releaseSFrameGroup(0, result);
}

// Drop the oldest once either cap is exceeded: it is least likely to still complete.
void VideoRtpDepacketizer::evictOverflowingSFrames() {
	while (!mSFrames.empty() &&
	       (mSFrames.size() > MaxSFrames || mSFrameBufferedBytes > MaxSFrameBufferedBytes)) {
		for (const auto &pkt : mSFrames.front().packets)
			mSFrameBufferedBytes -= pkt->size();
		mSFrames.erase(mSFrames.begin());
		COUNTER_SFRAME_INCOMPLETE++;
	}
}

void VideoRtpDepacketizer::incoming(message_vector &messages,
                                    [[maybe_unused]] const message_callback &send) {
	// One read for the whole batch, shared with the SFrame subclass that calls this.
	sframeBatch = enableSFrame.load(std::memory_order_relaxed);

	message_vector result;
	for (auto message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			continue;
		}

		if (message->size() < sizeof(RtpHeader)) {
			PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
			continue;
		}

		auto header = reinterpret_cast<const RtpHeader *>(message->data());
		if (message->size() < header->getSize())
			continue; // truncated header

		if (sframeBatch) {
			// Drop malformed SFrame packets (too small, truncated, invalid padding,
			// or no payload beyond the 1-byte descriptor) before buffering.
			size_t hdrSize = 0, payloadEnd = 0;
			if (!impl::SFrameUtility::ParseSFramePacket(message, hdrSize, payloadEnd))
				continue;

			const uint32_t timestamp = header->timestamp();
			if (isStaleSFrameTimestamp(timestamp)) {
				COUNTER_SFRAME_MALFORMED++;
				continue;
			}
			const bool endOfFrame = header->marker();
			bufferSFramePacket(std::move(message), timestamp);
			// The marker ends the frame, so this group is complete now. E still delimits
			// the objects inside it, which is what lets one timestamp carry several.
			if (endOfFrame)
				flushSFramesThrough(timestamp, result);
			evictOverflowingSFrames();
			continue;
		}

		if (!mBuffer.empty()) {
			auto first = *mBuffer.begin();
			auto firstHeader = reinterpret_cast<const RtpHeader *>(first->data());
			if (firstHeader->timestamp() != header->timestamp()) {
				if (auto frame = reassemble(mBuffer))
					result.push_back(frame);
				mBuffer.clear();
			}
		}

		mBuffer.insert(std::move(message));

		if (header->marker()) {
			if (auto frame = reassemble(mBuffer))
				result.push_back(std::move(frame));

			mBuffer.clear();
		}
	};

	// Everything available has been buffered, so any group that is no longer the newest is
	// finished and can be handed over, complete or not.
	if (sframeBatch) {
		flushSFramesExceptNewest(result);
		emitCompleteSFrames(result);
	}

	messages.swap(result);
}

bool VideoRtpDepacketizer::sequence_cmp::operator()(message_ptr a, message_ptr b) const {
	assert(a->size() >= sizeof(RtpHeader) && b->size() >= sizeof(RtpHeader));
	auto ha = reinterpret_cast<const rtc::RtpHeader *>(a->data());
	auto hb = reinterpret_cast<const rtc::RtpHeader *>(b->data());
	int16_t d = int16_t(hb->seqNumber() - ha->seqNumber());
	return d > 0;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
