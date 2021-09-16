/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef RTC_RTP_HPP
#define RTC_RTP_HPP

#include "common.hpp"

#include <vector>

namespace rtc {

typedef uint32_t SSRC;

#pragma pack(push, 1)

struct RTC_CPP_EXPORT RTP {
	uint8_t _first;
	uint8_t _payloadType;
	uint16_t _seqNumber;
	uint32_t _timestamp;
	SSRC _ssrc;
	SSRC _csrc[16];

	[[nodiscard]] uint8_t version() const;
	[[nodiscard]] bool padding() const;
	[[nodiscard]] bool extension() const;
	[[nodiscard]] uint8_t csrcCount() const;
	[[nodiscard]] uint8_t marker() const;
	[[nodiscard]] uint8_t payloadType() const;
	[[nodiscard]] uint16_t seqNumber() const;
	[[nodiscard]] uint32_t timestamp() const;
	[[nodiscard]] uint32_t ssrc() const;

	[[nodiscard]] size_t getSize() const;
	[[nodiscard]] const char *getBody() const;
	[[nodiscard]] char *getBody();

	void log() const;

	void preparePacket();
	void setSeqNumber(uint16_t newSeqNo);
	void setPayloadType(uint8_t newPayloadType);
	void setSsrc(uint32_t in_ssrc);
	void setMarker(bool marker);
	void setTimestamp(uint32_t i);
};

struct RTC_CPP_EXPORT RTCP_ReportBlock {
	SSRC _ssrc;
	uint32_t _fractionLostAndPacketsLost; // fraction lost is 8-bit, packets lost is 24-bit
	uint16_t _seqNoCycles;
	uint16_t _highestSeqNo;
	uint32_t _jitter;
	uint32_t _lastReport;
	uint32_t _delaySinceLastReport;

	[[nodiscard]] uint16_t seqNoCycles() const;
	[[nodiscard]] uint16_t highestSeqNo() const;
	[[nodiscard]] uint32_t jitter() const;
	[[nodiscard]] uint32_t delaySinceSR() const;

	[[nodiscard]] SSRC getSSRC() const;
	[[nodiscard]] uint32_t getNTPOfSR() const;
	[[nodiscard]] unsigned int getLossPercentage() const;
	[[nodiscard]] unsigned int getPacketLostCount() const;

	void preparePacket(SSRC in_ssrc, unsigned int packetsLost, unsigned int totalPackets,
	                   uint16_t highestSeqNo, uint16_t seqNoCycles, uint32_t jitter,
	                   uint64_t lastSR_NTP, uint64_t lastSR_DELAY);
	void setSSRC(SSRC in_ssrc);
	void setPacketsLost(unsigned int packetsLost, unsigned int totalPackets);
	void setSeqNo(uint16_t highestSeqNo, uint16_t seqNoCycles);
	void setJitter(uint32_t jitter);
	void setNTPOfSR(uint64_t ntp);
	void setDelaySinceSR(uint32_t sr);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_HEADER {
	uint8_t _first;
	uint8_t _payloadType;
	uint16_t _length;

	[[nodiscard]] uint8_t version() const;
	[[nodiscard]] bool padding() const;
	[[nodiscard]] uint8_t reportCount() const;
	[[nodiscard]] uint8_t payloadType() const;
	[[nodiscard]] uint16_t length() const;
	[[nodiscard]] size_t lengthInBytes() const;

	void prepareHeader(uint8_t payloadType, uint8_t reportCount, uint16_t length);
	void setPayloadType(uint8_t type);
	void setReportCount(uint8_t count);
	void setLength(uint16_t length);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_FB_HEADER {
	RTCP_HEADER header;

	SSRC _packetSender;
	SSRC _mediaSource;

	[[nodiscard]] SSRC packetSenderSSRC() const;
	[[nodiscard]] SSRC mediaSourceSSRC() const;

	void setPacketSenderSSRC(SSRC ssrc);
	void setMediaSourceSSRC(SSRC ssrc);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_SR {
	RTCP_HEADER header;

	SSRC _senderSSRC;
	uint64_t _ntpTimestamp;
	uint32_t _rtpTimestamp;
	uint32_t _packetCount;
	uint32_t _octetCount;

	RTCP_ReportBlock _reportBlocks;

	[[nodiscard]] static unsigned int Size(unsigned int reportCount);

	[[nodiscard]] uint64_t ntpTimestamp() const;
	[[nodiscard]] uint32_t rtpTimestamp() const;
	[[nodiscard]] uint32_t packetCount() const;
	[[nodiscard]] uint32_t octetCount() const;
	[[nodiscard]] uint32_t senderSSRC() const;

	[[nodiscard]] const RTCP_ReportBlock *getReportBlock(int num) const;
	[[nodiscard]] RTCP_ReportBlock *getReportBlock(int num);
	[[nodiscard]] unsigned int size(unsigned int reportCount);
	[[nodiscard]] size_t getSize() const;

	void preparePacket(SSRC senderSSRC, uint8_t reportCount);
	void setNtpTimestamp(uint64_t ts);
	void setRtpTimestamp(uint32_t ts);
	void setOctetCount(uint32_t ts);
	void setPacketCount(uint32_t ts);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_SDES_ITEM {
	uint8_t type;

	uint8_t _length;
	char _text[1];

	[[nodiscard]] static unsigned int Size(uint8_t textLength);

	[[nodiscard]] string text() const;
	[[nodiscard]] uint8_t length() const;

	void setText(string text);
};

struct RTCP_SDES_CHUNK {
	SSRC _ssrc;
	RTCP_SDES_ITEM _items;

	[[nodiscard]] static unsigned int Size(const std::vector<uint8_t> textLengths);

	[[nodiscard]] SSRC ssrc() const;

	void setSSRC(SSRC ssrc);

	// Get item at given index
	// All items with index < num must be valid, otherwise this function has undefined behaviour
	// (use safelyCountChunkSize() to check if chunk is valid).
	[[nodiscard]] const RTCP_SDES_ITEM *getItem(int num) const;
	[[nodiscard]] RTCP_SDES_ITEM *getItem(int num);

	// Get size of chunk
	// All items must be valid, otherwise this function has undefined behaviour (use
	// safelyCountChunkSize() to check if chunk is valid)
	[[nodiscard]] unsigned int getSize() const;

	long safelyCountChunkSize(size_t maxChunkSize) const;
};

struct RTC_CPP_EXPORT RTCP_SDES {
	RTCP_HEADER header;
	RTCP_SDES_CHUNK _chunks;

	[[nodiscard]] static unsigned int Size(const std::vector<std::vector<uint8_t>> lengths);

	bool isValid() const;

	// Returns number of chunks in this packet
	// Returns 0 if packet is invalid
	unsigned int chunksCount() const;

	// Get chunk at given index
	// All chunks (and their items) with index < `num` must be valid, otherwise this function has
	// undefined behaviour (use `isValid` to check if chunk is valid).
	const RTCP_SDES_CHUNK *getChunk(int num) const;
	RTCP_SDES_CHUNK *getChunk(int num);

	void preparePacket(uint8_t chunkCount);
};

struct RTC_CPP_EXPORT RTCP_RR {
	RTCP_HEADER header;

	SSRC _senderSSRC;
	RTCP_ReportBlock _reportBlocks;

	[[nodiscard]] static size_t SizeWithReportBlocks(uint8_t reportCount);

	SSRC senderSSRC() const;
	bool isSenderReport();
	bool isReceiverReport();

	[[nodiscard]] RTCP_ReportBlock *getReportBlock(int num);
	[[nodiscard]] const RTCP_ReportBlock *getReportBlock(int num) const;
	[[nodiscard]] size_t getSize() const;

	void preparePacket(SSRC senderSSRC, uint8_t reportCount);
	void setSenderSSRC(SSRC ssrc);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_REMB {
	RTCP_FB_HEADER header;

	char _id[4];       // Unique identifier ('R' 'E' 'M' 'B')
	uint32_t _bitrate; // Num SSRC, Br Exp, Br Mantissa (bit mask)
	SSRC _ssrc[1];

	[[nodiscard]] static size_t SizeWithSSRCs(int count);

	[[nodiscard]] unsigned int getSize() const;

	void preparePacket(SSRC senderSSRC, unsigned int numSSRC, unsigned int in_bitrate);
	void setBitrate(unsigned int numSSRC, unsigned int in_bitrate);
	void setSsrc(int iterator, SSRC newSssrc);
};

struct RTC_CPP_EXPORT RTCP_PLI {
	RTCP_FB_HEADER header;

	[[nodiscard]] static unsigned int Size();

	void preparePacket(SSRC messageSSRC);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_FIR_PART {
	uint32_t ssrc;
	uint8_t seqNo;
	uint8_t dummy1;
	uint16_t dummy2;
};

struct RTC_CPP_EXPORT RTCP_FIR {
	RTCP_FB_HEADER header;
	RTCP_FIR_PART parts[1];

	static unsigned int Size();

	void preparePacket(SSRC messageSSRC, uint8_t seqNo);

	void log() const;
};

struct RTC_CPP_EXPORT RTCP_NACK_PART {
	uint16_t _pid;
	uint16_t _blp;

	uint16_t pid();
	uint16_t blp();

	void setPid(uint16_t pid);
	void setBlp(uint16_t blp);

	std::vector<uint16_t> getSequenceNumbers();
};

struct RTC_CPP_EXPORT RTCP_NACK {
	RTCP_FB_HEADER header;
	RTCP_NACK_PART parts[1];

	[[nodiscard]] static unsigned int Size(unsigned int discreteSeqNoCount);

	[[nodiscard]] unsigned int getSeqNoCount();

	void preparePacket(SSRC ssrc, unsigned int discreteSeqNoCount);

	/**
	 * Add a packet to the list of missing packets.
	 * @param fciCount The number of FCI fields that are present in this packet.
	 *                  Let the number start at zero and let this function grow the number.
	 * @param fciPID The seq no of the active FCI. It will be initialized automatically, and will
	 * change automatically.
	 * @param missingPacket The seq no of the missing packet. This will be added to the queue.
	 * @return true if the packet has grown, false otherwise.
	 */
	bool addMissingPacket(unsigned int *fciCount, uint16_t *fciPID, uint16_t missingPacket);
};

struct RTC_CPP_EXPORT RTP_RTX {
	RTP header;

	[[nodiscard]] const char *getBody() const;
	[[nodiscard]] char *getBody();
	[[nodiscard]] size_t getBodySize(size_t totalSize) const;
	[[nodiscard]] size_t getSize() const;
	[[nodiscard]] uint16_t getOriginalSeqNo() const;

	// Returns the new size of the packet
	size_t normalizePacket(size_t totalSize, SSRC originalSSRC, uint8_t originalPayloadType);

	size_t copyTo(RTP *dest, size_t totalSize, uint8_t originalPayloadType);
};

#pragma pack(pop)

} // namespace rtc

#endif
