/**
 * Copyright (c) 2026 Kostya Vasilyev
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "xrmanager.hpp"

#if RTC_ENABLE_MEDIA

#include "logcounter.hpp"

#include <algorithm>
#include <utility>
#include <vector>

using namespace std::chrono;

namespace rtc::impl {

namespace {

constexpr auto kFlushInterval = seconds(1);
constexpr size_t kMaxPendingReporters = 100;
constexpr size_t kMaxSubBlocksPerPacket = 25;

LogCounter COUNTER_BAD_XR_HEADER(plog::warning, "Number of malformed RTCP XR headers");

} // namespace

void XrManager::incoming(const message_ptr &message) {
	if (!message || message->type != Message::Control)
		return;

	size_t offset = 0;
	while (offset + sizeof(RtcpHeader) <= message->size()) {
		auto header = reinterpret_cast<const RtcpHeader *>(message->data() + offset);
		size_t length = header->lengthInBytes();
		if (offset + length > message->size()) {
			COUNTER_BAD_XR_HEADER++;
			break;
		}

		if (header->payloadType() == 207 && length >= RtcpXr::HeaderSize()) {
			auto xr = reinterpret_cast<const RtcpXr *>(message->data() + offset);
			SSRC reporterSsrc = xr->senderSSRC();

			size_t blockOffset = offset + RtcpXr::HeaderSize();
			size_t blockEnd = offset + length;
			while (blockOffset + sizeof(RtcpXrBlockHeader) <= blockEnd) {
				auto blockHeader =
				    reinterpret_cast<const RtcpXrBlockHeader *>(message->data() + blockOffset);
				size_t blockLength = blockHeader->lengthInBytes();
				if (blockOffset + blockLength > blockEnd) {
					COUNTER_BAD_XR_HEADER++;
					break;
				}

				if (blockHeader->blockType() == 4 && blockLength >= RtcpXrRrtrBlock::Size()) {
					auto rrtr = reinterpret_cast<const RtcpXrRrtrBlock *>(message->data() + blockOffset);

					std::lock_guard lock(mMutex);
					auto it = mPending.find(reporterSsrc);
					if (it != mPending.end()) {
						it->second = {rrtr->ntpTimestamp(), steady_clock::now()};
					} else if (mPending.size() < kMaxPendingReporters) {
						mPending.emplace(reporterSsrc,
						                  PendingReport{rrtr->ntpTimestamp(), steady_clock::now()});
					}
				}

				blockOffset += blockLength;
			}
		}

		offset += length;
	}
}

void XrManager::send(SSRC localSsrc, const message_callback &sendCallback) {
	std::vector<std::pair<SSRC, PendingReport>> due;
	{
		std::lock_guard lock(mMutex);
		if (mPending.empty())
			return;

		auto now = steady_clock::now();
		if (now - mLastFlush < kFlushInterval)
			return;

		mLastFlush = now;
		due.assign(mPending.begin(), mPending.end());
		mPending.clear();
	}

	for (size_t start = 0; start < due.size(); start += kMaxSubBlocksPerPacket) {
		size_t count = std::min(kMaxSubBlocksPerPacket, due.size() - start);

		size_t xrSize = RtcpXr::HeaderSize() + RtcpXrDlrrBlock::SizeWithSubBlocks(int(count));
		auto message = make_message(xrSize, Message::Control);

		auto dlrr = reinterpret_cast<RtcpXrDlrrBlock *>(message->data() + RtcpXr::HeaderSize());
		dlrr->preparePacket(int(count));

		auto now = steady_clock::now();
		for (size_t i = 0; i < count; ++i) {
			const auto &[reporterSsrc, report] = due[start + i];
			uint32_t lrr = uint32_t(report.ntpTimestamp >> 16);
			uint32_t delay =
			    uint32_t(duration_cast<microseconds>(now - report.receivedAt).count() * 65536 / 1000000);
			dlrr->getSubBlock(int(i))->preparePacket(reporterSsrc, lrr, delay);
		}

		auto xr = reinterpret_cast<RtcpXr *>(message->data());
		xr->preparePacket(localSsrc, uint16_t(xrSize / 4 - 1));

		sendCallback(message);
	}
}

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA
