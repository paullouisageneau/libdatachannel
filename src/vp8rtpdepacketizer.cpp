#if RTC_ENABLE_MEDIA

#include "vp8rtpdepacketizer.hpp"
#include "impl/internals.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace rtc {

void VP8RtpDepacketizer::incoming(message_vector &messages, const message_callback &) {
	// Move all non-control messages into mRtpBuffer
	messages.erase(std::remove_if(messages.begin(), messages.end(),
	                              [&](message_ptr msg) {
		                              if (msg->type == Message::Control)
			                              return false; // keep
		                              if (msg->size() < sizeof(RtpHeader)) {
			                              PLOG_VERBOSE << "Dropping too-short RTP packet, size="
			                                           << msg->size();
			                              return true;
		                              }
		                              mRtpBuffer.push_back(std::move(msg));
		                              return true;
	                              }),
	               messages.end());

	// Process RTP packets
	while (!mRtpBuffer.empty()) {
		// Get the timestamp of the first packet
		const auto &front = mRtpBuffer.front();
		auto rh = reinterpret_cast<const RtpHeader *>(front->data());
		uint32_t ts = rh->timestamp();
		uint8_t pt = rh->payloadType();

		// Collect all packets with the same timestamp
		std::vector<message_ptr> framePackets;
		bool markerFound = false;
		uint16_t expectedSeq = rh->seqNumber();
		bool missingPackets = false;

		for (auto it = mRtpBuffer.begin(); it != mRtpBuffer.end();) {
			auto hdr = reinterpret_cast<const RtpHeader *>((*it)->data());
			if (hdr->timestamp() == ts) {
				if (hdr->seqNumber() != expectedSeq) {
					missingPackets = true;
					break;
				}
				framePackets.push_back(std::move(*it));
				if (hdr->marker())
					markerFound = true;
				expectedSeq++; // handles wrap-around
				it = mRtpBuffer.erase(it);
			} else {
				++it;
			}
		}

		if (missingPackets || !markerFound) {
			// Missing packets or frame not complete yet, wait for more packets
			// Put back the packets we removed
			mRtpBuffer.insert(mRtpBuffer.begin(), framePackets.begin(), framePackets.end());
			// Exit the loop to wait for more packets to arrive
			break;
		}

		// Now build the frame
		auto frames = buildFrame(framePackets.begin(), framePackets.end(), pt, ts);
		for (auto &f : frames)
			messages.push_back(std::move(f));
	}
}

message_vector VP8RtpDepacketizer::buildFrame(
	std::vector<message_ptr>::iterator first,
	std::vector<message_ptr>::iterator last,
	uint8_t payloadType,
	uint32_t timestamp)
{
	// Sort by ascending sequence number
	std::sort(first, last, [&](const message_ptr &a, const message_ptr &b){
		auto ha = reinterpret_cast<const RtpHeader*>(a->data());
		auto hb = reinterpret_cast<const RtpHeader*>(b->data());
		return seqLess(ha->seqNumber(), hb->seqNumber());
	});

	binary frameData;
	// Track whether we ever see S=1 and PID=0, to detect a valid first partition
	bool foundPartition0 = false;

	for(auto it = first; it != last; ++it) {
		auto &pkt = *it;
		auto hdr = reinterpret_cast<const RtpHeader*>(pkt->data());

		// The VP8 payload descriptor is after the RTP header:
		size_t hdrSize = hdr->getSize() + hdr->getExtensionHeaderSize();
		size_t totalSize = pkt->size();
		if(totalSize <= hdrSize) {
			// std::cout
			// 	<< "[VP8RtpDepacketizer] Packet seq=" << seq
			// 	<< " is too small to contain VP8 data! Dropping.\n";
			continue;
		}

		// Create a NalUnit object that contains the raw payload (minus RTP header).
		VP8NalUnit nal{binary(pkt->begin() + hdrSize, pkt->end())};
		// Let it parse the VP8 descriptor bits.
		size_t descLen = nal.parseDescriptor();

		bool Sbit = nal.isStartOfPartition(); // The S bit
		// There's no direct method named partitionIndex() in your snippet, but you can do:
		uint8_t pid  = nal.mFirstByte.partitionIndex(); // The low 3 bits from raw data

		// For a valid first partition, we want S=1 and PID=0
		if(Sbit && pid == 0) {
			foundPartition0 = true;
		}

		// Now strip off the descriptor bytes and append the underlying VP8 bitstream:
		size_t payloadSize = totalSize - hdrSize; // how many bytes of actual payload
		if(descLen > payloadSize) {
			// means parseDescriptor said there's more descriptor bytes than we have
			// std::cout
			// 	<< "[VP8RtpDepacketizer] parseDescriptor length=" << descLen
			// 	<< " > actual payload=" << payloadSize
			// 	<< ", skipping.\n";
			continue;
		}

		// The actual compressed data is after the descriptor
		size_t offset = hdrSize + descLen;
		size_t copyLen = totalSize - offset;
		frameData.insert(frameData.end(),
						 pkt->begin() + offset,
						 pkt->begin() + offset + copyLen);
	}

	// Now check if there is at least one packet with S=1 & PID=0:
	if(!foundPartition0) {
		// std::cout
		// 	<< "[VP8RtpDepacketizer] WARNING: No S=1,PID=0 found for TS=" << timestamp
		// 	<< " => incomplete or invalid frame. Discarding.\n";
		// Return empty so we do NOT feed a broken frame to FFmpeg.
		return {};
	}

	// Log if the last packet had M=1 (which means end of frame)
	auto lastPktHeader = reinterpret_cast<const RtpHeader*>((last-1)->get()->data());
	bool lastMarker = lastPktHeader->marker();
	// std::cout
	// 	<< "[VP8RtpDepacketizer] Done building frame TS=" << timestamp
	// 	<< ", final size=" << frameData.size()
	// 	<< ", last packet's marker=" << (lastMarker ? 1 : 0)
	// 	<< std::endl;

	// If you only want to return the frame if M=1 is set, do that check here:
	if(!lastMarker) {
		// std::cout << "[VP8RtpDepacketizer] lastMarker=0, discarding partial frame.\n";
		return {};
	}

	// Normal return
	message_vector out;
	if(!frameData.empty()) {
		auto finfo = std::make_shared<FrameInfo>(timestamp);
		finfo->timestampSeconds =
			std::chrono::duration<double>(double(timestamp) / double(ClockRate));
		finfo->payloadType = payloadType;

		auto msg = make_message(std::move(frameData), finfo);
		out.push_back(std::move(msg));
	}
	return out;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
