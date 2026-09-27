/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "sframeperframevideortpdepacketizer.hpp"
#include "rtp.hpp"

#include "impl/internals.hpp"
#include "impl/logcounter.hpp"
#include "impl/sframecodec.hpp"
#include "impl/sframeutility.hpp"

#include <algorithm>

namespace rtc {

static impl::LogCounter
    COUNTER_SFRAME_INCOMPLETE(plog::warning,
                              "Number of incomplete SFrame frames dropped from the buffer");
static impl::LogCounter COUNTER_SFRAME_MALFORMED(plog::warning,
                                                 "Number of malformed SFrame frames dropped");
static impl::LogCounter
    COUNTER_SFRAME_PACKET_LOSS(plog::warning,
                               "Number of SFrame objects dropped because an RTP packet was lost");
static impl::LogCounter
    COUNTER_SFRAME_PACKETIZED_ORIGIN(plog::warning,
                                     "Number of SFrame video packets dropped for unsupported "
                                     "per-packet SFrame (descriptor T=1)");

// The peer decides how much is held here, so cap both. The stream count is capped too, by the
// shared impl::sframe::MaxSFrameStreams.
const size_t MaxSFrames = 8;
const size_t MaxSFrameBufferedBytes = 4 * 1024 * 1024;

// The per-packet completeness walk covers a whole group, so an unbounded group turns it
// quadratic. The byte budget does not bound it, since the smallest packet worth buffering is an
// RTP header. A real frame cannot span this many packets.
const size_t MaxSFramePacketsPerGroup = 2048;

SFramePerFrameVideoRtpDepacketizer::SFramePerFrameVideoRtpDepacketizer(
    shared_ptr<SFrameReceiveKeyProvider> keyProvider)
    : mSFrameDecoders(std::make_unique<impl::SFrameDecoderSet>(std::move(keyProvider))) {}

SFramePerFrameVideoRtpDepacketizer::~SFramePerFrameVideoRtpDepacketizer() {}

// Checks one delimited run and returns the total payload size, or nullopt if it is not a
// well-formed object. Kept separate from assembly so the completeness check can run without
// copying: it runs on every buffered group on every packet.
optional<size_t> SFramePerFrameVideoRtpDepacketizer::validateSFrameRun(const message_vector &run) {
	size_t total = 0;
	uint8_t pt = 0;
	const size_t count = run.size();
	for (size_t idx = 0; idx < count; ++idx) {
		const auto &pkt = run[idx];
		size_t hdrSize = 0, payloadEnd = 0;
		if (!impl::sframe::ParseSFramePacket(pkt, hdrSize, payloadEnd))
			return nullopt;

		const uint8_t descriptor = static_cast<uint8_t>(*(pkt->begin() + hdrSize));
		if (!impl::sframe::IsValidDescriptor(descriptor, idx == 0, idx + 1 == count))
			return nullopt;

		// draft-ietf-avtcore-rtp-sframe section 5.2: the payload type must be the same across
		// the run, or the object spans two encodings.
		auto header = reinterpret_cast<const RtpHeader *>(pkt->data());
		if (idx == 0)
			pt = header->payloadType();
		else if (header->payloadType() != pt)
			return nullopt;

		total += payloadEnd - (hdrSize + 1);
	}

	if (total == 0)
		return nullopt;

	return total;
}

// Concatenates one delimited run's payloads, descriptor and padding removed.
bool SFramePerFrameVideoRtpDepacketizer::assembleSFrameObject(const message_vector &run,
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
		// Checked above by validateSFrameRun(), but the result is honoured rather than discarded:
		// on a false return both offsets stay 0 and the range below would run backwards.
		if (!impl::sframe::ParseSFramePacket(pkt, hdrSize, payloadEnd))
			return false;

		assembled.insert(assembled.end(), pkt->begin() + hdrSize + 1, pkt->begin() + payloadEnd);
	}

	result.push_back(make_message(std::move(assembled), createFrameInfo(ts, pt)));
	return true;
}

// draft-ietf-avtcore-rtp-sframe section 5.2: an object is the smallest run of consecutive
// packets from S to E, so one timestamp can hold several. Runs must also be contiguous in
// sequence number, or an object missing a middle packet would be spliced together.
// True when every packet in the group belongs to a complete S...E run, so nothing further
// is expected for this timestamp.
bool SFramePerFrameVideoRtpDepacketizer::sframeGroupResolves(const message_buffer &packets) {
	message_vector discard;
	return assembleSFrameObjects(packets, discard, /*dryRun=*/true).allConsumed;
}

SFramePerFrameVideoRtpDepacketizer::SFrameOutcome
SFramePerFrameVideoRtpDepacketizer::assembleSFrameObjects(const message_buffer &packets,
                                                          message_vector &result, bool dryRun) {
	message_vector staged;
	SFrameOutcome outcome;
	message_vector run;
	uint16_t expectedSeq = 0;

	for (const auto &pkt : packets) {
		auto header = reinterpret_cast<const RtpHeader *>(pkt->data());

		// No payload once padding is removed, so it is part of no object -- but it did take a
		// sequence number, and stepping over it here is the whole difference between padding
		// and a lost packet. The codec depacketizers skip such a packet the same way.
		if (impl::sframe::HasNoRtpPayload(*pkt)) {
			if (header->seqNumber() == expectedSeq)
				expectedSeq = static_cast<uint16_t>(header->seqNumber() + 1);
			continue;
		}

		size_t hdrSize = 0, payloadEnd = 0;
		if (!impl::sframe::ParseSFramePacket(pkt, hdrSize, payloadEnd)) {
			outcome.allConsumed = false;
			outcome.sawMalformed = true;
			run.clear();
			continue;
		}

		const uint8_t descriptor = static_cast<uint8_t>(*(pkt->begin() + hdrSize));

		// Counted separately: T=1 is a mode we decline, not corruption.
		if (impl::sframe::IsPacketizedOrigin(descriptor)) {
			COUNTER_SFRAME_PACKETIZED_ORIGIN++;
			outcome.allConsumed = false;
			run.clear();
			continue;
		}

		if (descriptor & impl::SFrameDescriptorS) {
			// A new object starts here, so anything still open lost its E packet.
			if (!run.empty()) {
				outcome.allConsumed = false;
				outcome.sawLoss = true;
			}
			run.clear();
		} else if (run.empty() || header->seqNumber() != expectedSeq) {
			// A continuation whose S never arrived, or a gap mid-run. Every packet that did
			// arrive is accounted for above, so either way a packet was lost.
			outcome.allConsumed = false;
			outcome.sawLoss = true;
			run.clear();
			continue;
		}

		run.push_back(pkt);
		expectedSeq = static_cast<uint16_t>(header->seqNumber() + 1);

		if (!(descriptor & impl::SFrameDescriptorE))
			continue;

		// The dry run only asks whether the group is complete, so it skips the copy.
		const bool ok =
		    dryRun ? validateSFrameRun(run).has_value() : assembleSFrameObject(run, staged);
		if (!ok) {
			outcome.allConsumed = false;
			outcome.sawMalformed = true;
		}
		run.clear();
	}

	if (!run.empty()) {
		outcome.allConsumed = false; // an object whose E packet never arrived
		outcome.sawLoss = true;
	}

	if (dryRun)
		return outcome;

	if (outcome.sawLoss)
		COUNTER_SFRAME_PACKET_LOSS++;
	if (outcome.sawMalformed)
		COUNTER_SFRAME_MALFORMED++;

	result.insert(result.end(), std::make_move_iterator(staged.begin()),
	              std::make_move_iterator(staged.end()));
	return outcome;
}

// A packet older than the last group delivered can no longer be reassembled into it, and buffering
// it would create a group at the head that never completes. Per stream, wrap-safe as for sequence
// numbers, and equal timestamps are not stale: several SFrame objects may share one.
bool SFramePerFrameVideoRtpDepacketizer::isStaleSFrameTimestamp(const StreamState &stream,
                                                                uint32_t timestamp) const {
	return stream.emitted && int32_t(timestamp - stream.lastEmitted) < 0;
}

SFramePerFrameVideoRtpDepacketizer::StreamState &
SFramePerFrameVideoRtpDepacketizer::streamFor(SSRC ssrc) {
	// Bounded here, where the entry is created, rather than in evictOverflowingSFrames(): a packet
	// dropped before it is buffered never reaches that call, so a peer putting a fresh SSRC on
	// every such packet would grow this map without limit. The stream idle longest goes first, and
	// never the one being asked for.
	while (mStreams.size() >= impl::sframe::MaxSFrameStreams &&
	       mStreams.find(ssrc) == mStreams.end()) {
		auto idlest = mStreams.begin();
		for (auto it = mStreams.begin(); it != mStreams.end(); ++it)
			if (it->second.lastArrival < idlest->second.lastArrival)
				idlest = it;

		// Its groups still hold part of the shared byte budget.
		while (!idlest->second.groups.empty()) {
			dropSFrameGroup(idlest->second, 0);
			COUNTER_SFRAME_INCOMPLETE++;
		}
		mStreams.erase(idlest);
	}

	auto &stream = mStreams[ssrc];
	stream.lastArrival = ++mSFrameArrivals;
	return stream;
}

// Forgets a group without emitting it, keeping the shared byte total in step.
void SFramePerFrameVideoRtpDepacketizer::dropSFrameGroup(StreamState &stream, size_t index) {
	for (const auto &pkt : stream.groups[index].packets)
		mSFrameBufferedBytes -= pkt->size();
	stream.groups.erase(stream.groups.begin() + index);
}

// Hands one buffered group over: whatever forms complete S...E runs is emitted, the rest is
// dropped as unrecoverable.
void SFramePerFrameVideoRtpDepacketizer::releaseSFrameGroup(StreamState &stream, size_t index,
                                                            message_vector &result) {
	assembleSFrameObjects(stream.groups[index].packets, result);
	dropSFrameGroup(stream, index);
}

// Hand over every group up to and including `timestamp`, complete or not. Mirrors the
// codec path, which finishes a frame on the marker bit as well as on a timestamp change.
void SFramePerFrameVideoRtpDepacketizer::flushSFramesThrough(StreamState &stream,
                                                             uint32_t timestamp,
                                                             message_vector &result) {
	while (!stream.groups.empty()) {
		const bool last = stream.groups.front().timestamp == timestamp;
		releaseSFrameGroup(stream, 0, result);
		if (last)
			break;
	}
}

// Once a newer group has been buffered, every earlier one holds all the packets it is going to get,
// so hand them over even if one is incomplete: this bounds head-of-line blocking for a sender that
// leaves the marker clear. Newest by arrival, not by timestamp -- ordering by timestamp would let
// one injected packet with a far-future timestamp stay newest for ever and flush every genuine
// frame behind it incomplete. The cost is that cross-frame reordering can lose a frame, as it can
// for the codec depacketizers.
void SFramePerFrameVideoRtpDepacketizer::flushSFramesExceptNewest(StreamState &stream,
                                                                  message_vector &result) {
	while (stream.groups.size() > 1)
		releaseSFrameGroup(stream, 0, result);
}

// True when the packet was stored, which is also the only case that can make a group newly
// resolvable.
bool SFramePerFrameVideoRtpDepacketizer::bufferSFramePacket(StreamState &stream, message_ptr packet,
                                                            uint32_t timestamp) {
	auto &groups = stream.groups;
	auto it = std::find_if(groups.begin(), groups.end(),
	                       [timestamp](const SFrameGroup &g) { return g.timestamp == timestamp; });
	if (it == groups.end())
		it = groups.insert(groups.end(), SFrameGroup{timestamp, ++mSFrameArrivals, {}});

	// A duplicate sequence number is not stored, so only count what the set accepted:
	// otherwise the byte total drifts up permanently and the caps fire with nothing buffered.
	const size_t size = packet->size();
	if (!it->packets.insert(std::move(packet)).second)
		return false;

	mSFrameBufferedBytes += size;
	return true;
}

// Emits, in arrival order, every leading group whose packets already form whole S...E runs.
// Completion is judged from the E bit rather than the RTP marker: the marker is
// profile-specific, while every SFrame packet carries a descriptor.
void SFramePerFrameVideoRtpDepacketizer::emitCompleteSFrames(StreamState &stream,
                                                             message_vector &result) {
	while (!stream.groups.empty() && sframeGroupResolves(stream.groups.front().packets))
		releaseSFrameGroup(stream, 0, result);
}

// Drop the oldest once a cap is exceeded: it is least likely to still complete. The byte
// budget and the stream count are totals, so announcing more SSRCs cannot multiply either.
void SFramePerFrameVideoRtpDepacketizer::evictOverflowingSFrames() {
	// A group over the packet cap is dropped whole: it cannot be a real frame.
	for (auto &entry : mStreams) {
		auto &groups = entry.second.groups;
		for (size_t i = 0; i < groups.size();) {
			if (groups[i].packets.size() > MaxSFramePacketsPerGroup) {
				dropSFrameGroup(entry.second, i);
				COUNTER_SFRAME_INCOMPLETE++;
			} else {
				++i;
			}
		}
	}

	for (auto &entry : mStreams) {
		while (entry.second.groups.size() > MaxSFrames) {
			dropSFrameGroup(entry.second, 0);
			COUNTER_SFRAME_INCOMPLETE++;
		}
	}

	// Then the oldest group anywhere, until the shared byte budget is met.
	while (mSFrameBufferedBytes > MaxSFrameBufferedBytes) {
		StreamState *oldest = nullptr;
		for (auto &entry : mStreams) {
			if (entry.second.groups.empty())
				continue;
			if (!oldest || entry.second.groups.front().arrival < oldest->groups.front().arrival)
				oldest = &entry.second;
		}
		if (!oldest)
			break; // nothing buffered, so the total is stale rather than over budget

		dropSFrameGroup(*oldest, 0);
		COUNTER_SFRAME_INCOMPLETE++;
	}
}

void SFramePerFrameVideoRtpDepacketizer::incoming(message_vector &messages,
                                                  [[maybe_unused]] const message_callback &send) {
	// Captured before reassembly strips it, for the per-SSRC derivation of
	// draft-ietf-avtcore-rtp-sframe section 7: FrameInfo does not carry it. One SSRC per call,
	// since PeerConnection routes each packet by SSRC, but reassembly stays per stream because the
	// buffer outlives the call.
	bool ssrcValid = false;
	SSRC ssrc = 0;
	StreamState *stream = nullptr;
	message_vector result;

	// The SSRC selects the key only with per-SSRC derivation on, so only then does a batch
	// carrying several have to be split by it. Tracked only in that mode.
	const bool perSsrcKeys = mSFrameDecoders->usePerSSRCDerivation();
	std::vector<SSRC> owners;
	SSRC firstOwner = 0;
	bool severalOwners = false;
	auto tagAppended = [&](SSRC owner) {
		if (!perSsrcKeys)
			return;
		if (owner && result.size() > owners.size()) {
			if (!firstOwner)
				firstOwner = owner;
			else if (owner != firstOwner)
				severalOwners = true;
		}
		owners.resize(result.size(), owner);
	};

	// Only a stored packet can make a group newly resolvable, so a duplicate does not earn a
	// completeness walk over the whole group.
	bool stored = false;
	for (auto message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			tagAppended(0); // Control never decrypts
			continue;
		}

		if (message->size() < sizeof(RtpHeader)) {
			PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
			continue;
		}

		auto header = reinterpret_cast<const RtpHeader *>(message->data());
		if (message->size() < header->getSize())
			continue; // truncated header

		ssrcValid = true;
		ssrc = header->ssrc();
		stream = &streamFor(ssrc);

		// Malformed (too small, truncated, invalid padding, or no payload beyond the 1-byte
		// descriptor) is dropped. A packet with no payload at all is buffered like any other:
		// it belongs to no object, but the run walker has to see the sequence number it took,
		// or padding is indistinguishable from a lost packet.
		size_t hdrSize = 0, payloadEnd = 0;
		if (!impl::sframe::ParseSFramePacket(message, hdrSize, payloadEnd) &&
		    !impl::sframe::HasNoRtpPayload(*message))
			continue;

		const uint32_t timestamp = header->timestamp();
		if (isStaleSFrameTimestamp(*stream, timestamp)) {
			COUNTER_SFRAME_MALFORMED++;
			continue;
		}

		const bool endOfFrame = header->marker();
		stored |= bufferSFramePacket(*stream, std::move(message), timestamp);
		// The marker ends the frame, so this group is complete now. E still delimits the
		// objects inside it, which is what lets one timestamp carry several.
		if (endOfFrame) {
			flushSFramesThrough(*stream, timestamp, result);
			tagAppended(ssrc);
		}
		evictOverflowingSFrames();
	}

	// Everything available has been buffered, so any group that is no longer the newest is
	// finished and can be handed over, complete or not. Only this call's stream is flushed:
	// what it emits is decrypted below under this call's SSRC, and another stream's frames
	// would need that stream's key.
	if (stream) {
		flushSFramesExceptNewest(*stream, result);
		if (stored)
			emitCompleteSFrames(*stream, result);
		tagAppended(ssrc);
	}
	messages.swap(result);

	if (!ssrcValid) {
		// No SSRC to derive the per-SSRC key from: drop the media, fail closed. RTCP passes.
		messages.erase(
		    std::remove_if(messages.begin(), messages.end(),
		                   [](const message_ptr &m) { return m->type != Message::Control; }),
		    messages.end());
		return;
	}

	if (severalOwners)
		impl::sframe::DecryptMessages(messages, owners, *mSFrameDecoders);
	else
		impl::sframe::DecryptMessages(messages, ssrc, *mSFrameDecoders);

	// Only a frame that authenticated may move the staleness watermark: a forged packet that merely
	// forms an S..E run would otherwise set it far ahead and make every genuine packet behind it
	// look like a late straggler. Newest wins, compared as a signed difference so it survives the
	// 32-bit wrap.
	for (const auto &msg : messages) {
		if (msg->type == Message::Control || !msg->frameInfo)
			continue;

		const uint32_t timestamp = msg->frameInfo->timestamp;
		if (!stream->emitted || int32_t(timestamp - stream->lastEmitted) > 0) {
			stream->lastEmitted = timestamp;
			stream->emitted = true;
		}
	}
}

message_ptr SFramePerFrameVideoRtpDepacketizer::reassemble(message_buffer &) {
	// Unreachable. reassemble() is the codec hook VideoRtpDepacketizer::incoming() calls, and this
	// overrides incoming() outright -- every packet goes through the SFrame group assembler
	// instead. It existed to concatenate plain payloads when a peer declined a=sframe; that state
	// no longer exists, since a declined m-line is stopped rather than downgraded. The override
	// stays only because the base declares it pure virtual.
	return nullptr;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
