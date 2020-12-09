/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#include "log.hpp"

#include <cmath>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifndef htonll
#define htonll(x)                                                                                  \
	((uint64_t)htonl(((uint64_t)(x)&0xFFFFFFFF) << 32) | (uint64_t)htonl((uint64_t)(x) >> 32))
#endif
#ifndef ntohll
#define ntohll(x) htonll(x)
#endif

namespace rtc {

typedef uint32_t SSRC;

#pragma pack(push, 1)

struct RTP {
private:
	uint8_t _first;
	uint8_t _payloadType;
	uint16_t _seqNumber;
	uint32_t _timestamp;
	SSRC _ssrc;

public:
	SSRC csrc[16];

	inline uint8_t version() const { return _first >> 6; }
	inline bool padding() const { return (_first >> 5) & 0x01; }
	inline bool extension() const { return (_first >> 4) & 0x01; }
	inline uint8_t csrcCount() const { return _first & 0x0F; }
	inline uint8_t marker() const { return _payloadType & 0b10000000; }
	inline uint8_t payloadType() const { return _payloadType & 0b01111111; }
	inline uint16_t seqNumber() const { return ntohs(_seqNumber); }
	inline uint32_t timestamp() const { return ntohl(_timestamp); }
	inline uint32_t ssrc() const { return ntohl(_ssrc); }

	inline size_t getSize() const {
		return reinterpret_cast<const char *>(&csrc) - reinterpret_cast<const char *>(this) +
		       sizeof(SSRC) * csrcCount();
	}

	[[nodiscard]] char *getBody() {
		return reinterpret_cast<char *>(&csrc) + sizeof(SSRC) * csrcCount();
	}

	[[nodiscard]] const char *getBody() const {
		return reinterpret_cast<const char *>(&csrc) + sizeof(SSRC) * csrcCount();
	}

	inline void setSeqNumber(uint16_t newSeqNo) { _seqNumber = htons(newSeqNo); }
	inline void setPayloadType(uint8_t newPayloadType) {
		_payloadType = (_payloadType & 0b10000000u) | (0b01111111u & newPayloadType);
	}
	inline void setSsrc(uint32_t in_ssrc) { _ssrc = htonl(in_ssrc); }

	void setTimestamp(uint32_t i) { _timestamp = htonl(i); }

	void log() {
		PLOG_VERBOSE << "RTP V: " << (int)version() << " P: " << (padding() ? "P" : " ")
		             << " X: " << (extension() ? "X" : " ") << " CC: " << (int)csrcCount()
		             << " M: " << (marker() ? "M" : " ") << " PT: " << (int)payloadType()
		             << " SEQNO: " << seqNumber() << " TS: " << timestamp();
	}
};

struct RTCP_ReportBlock {
	SSRC ssrc;

private:
	uint32_t _fractionLostAndPacketsLost; // fraction lost is 8-bit, packets lost is 24-bit
	uint16_t _seqNoCycles;
	uint16_t _highestSeqNo;
	uint32_t _jitter;
	uint32_t _lastReport;
	uint32_t _delaySinceLastReport;

public:
	inline void preparePacket(SSRC in_ssrc, [[maybe_unused]] unsigned int packetsLost,
	                          [[maybe_unused]] unsigned int totalPackets, uint16_t highestSeqNo,
	                          uint16_t seqNoCycles, uint32_t jitter, uint64_t lastSR_NTP,
	                          uint64_t lastSR_DELAY) {
		setSeqNo(highestSeqNo, seqNoCycles);
		setJitter(jitter);
		setSSRC(in_ssrc);

		// Middle 32 bits of NTP Timestamp
		//		  this->lastReport = lastSR_NTP >> 16u;
		setNTPOfSR(uint64_t(lastSR_NTP));
		setDelaySinceSR(uint32_t(lastSR_DELAY));

		// The delay, expressed in units of 1/65536 seconds
		// this->delaySinceLastReport = lastSR_DELAY;
	}

	inline void setSSRC(SSRC in_ssrc) { this->ssrc = htonl(in_ssrc); }
	[[nodiscard]] inline SSRC getSSRC() const { return ntohl(ssrc); }

	inline void setPacketsLost([[maybe_unused]] unsigned int packetsLost,
	                           [[maybe_unused]] unsigned int totalPackets) {
		// TODO Implement loss percentages.
		_fractionLostAndPacketsLost = 0;
	}

	[[nodiscard]] inline unsigned int getLossPercentage() const {
		// TODO Implement loss percentages.
		return 0;
	}
	[[nodiscard]] inline unsigned int getPacketLostCount() const {
		// TODO Implement total packets lost.
		return 0;
	}

	inline uint16_t seqNoCycles() const { return ntohs(_seqNoCycles); }
	inline uint16_t highestSeqNo() const { return ntohs(_highestSeqNo); }
	inline uint32_t jitter() const { return ntohl(_jitter); }

	inline void setSeqNo(uint16_t highestSeqNo, uint16_t seqNoCycles) {
		_highestSeqNo = htons(highestSeqNo);
		_seqNoCycles = htons(seqNoCycles);
	}

	inline void setJitter(uint32_t jitter) { _jitter = htonl(jitter); }

	inline void setNTPOfSR(uint64_t ntp) { _lastReport = htonll(ntp >> 16u); }
	[[nodiscard]] inline uint32_t getNTPOfSR() const { return ntohl(_lastReport) << 16u; }

	inline void setDelaySinceSR(uint32_t sr) {
		// The delay, expressed in units of 1/65536 seconds
		_delaySinceLastReport = htonl(sr);
	}
	[[nodiscard]] inline uint32_t getDelaySinceSR() const { return ntohl(_delaySinceLastReport); }

	inline void log() const {
		PLOG_VERBOSE << "RTCP report block: "
		             << "ssrc="
		             << ntohl(ssrc)
		             // TODO: Implement these reports
		             //	<< ", fractionLost=" << fractionLost
		             //	<< ", packetsLost=" << packetsLost
		             << ", highestSeqNo=" << highestSeqNo() << ", seqNoCycles=" << seqNoCycles()
		             << ", jitter=" << jitter() << ", lastSR=" << getNTPOfSR()
		             << ", lastSRDelay=" << getDelaySinceSR();
	}
};

struct RTCP_HEADER {
private:
	uint8_t _first;
	uint8_t _payloadType;
	uint16_t _length;

public:
	inline uint8_t version() const { return _first >> 6; }
	inline bool padding() const { return (_first >> 5) & 0x01; }
	inline uint8_t reportCount() const { return _first & 0x0F; }
	inline uint8_t payloadType() const { return _payloadType; }
	inline uint16_t length() const { return ntohs(_length); }
	inline size_t lengthInBytes() const { return (1 + length()) * 4; }

	inline void setPayloadType(uint8_t type) { _payloadType = type; }
	inline void setReportCount(uint8_t count) {
		_first = (_first & 0b11100000u) | (count & 0b00011111u);
	}
	inline void setLength(uint16_t length) { _length = htons(length); }

	inline void prepareHeader(uint8_t payloadType, uint8_t reportCount, uint16_t length) {
		_first = 0b10000000; // version 2, no padding
		setReportCount(reportCount);
		setPayloadType(payloadType);
		setLength(length);
	}

	inline void log() const {
		PLOG_VERBOSE << "RTCP header: "
		             << "version=" << unsigned(version()) << ", padding=" << padding()
		             << ", reportCount=" << unsigned(reportCount())
		             << ", payloadType=" << unsigned(payloadType()) << ", length=" << length();
	}
};

struct RTCP_FB_HEADER {
	RTCP_HEADER header;
	SSRC packetSender;
	SSRC mediaSource;

	[[nodiscard]] SSRC getPacketSenderSSRC() const { return ntohl(packetSender); }

	[[nodiscard]] SSRC getMediaSourceSSRC() const { return ntohl(mediaSource); }

	void setPacketSenderSSRC(SSRC ssrc) { this->packetSender = htonl(ssrc); }

	void setMediaSourceSSRC(SSRC ssrc) { this->mediaSource = htonl(ssrc); }

	void log() {
		header.log();
		PLOG_VERBOSE << "FB: "
		             << " packet sender: " << getPacketSenderSSRC()
		             << " media source: " << getMediaSourceSSRC();
	}
};

struct RTCP_SR {
	RTCP_HEADER header;
	SSRC _senderSSRC;

private:
	uint64_t _ntpTimestamp;
	uint32_t _rtpTimestamp;
	uint32_t _packetCount;
	uint32_t _octetCount;

	RTCP_ReportBlock _reportBlocks;

public:
	inline void preparePacket(SSRC senderSSRC, uint8_t reportCount) {
		unsigned int length =
		    ((sizeof(header) + 24 + reportCount * sizeof(RTCP_ReportBlock)) / 4) - 1;
		header.prepareHeader(200, reportCount, uint16_t(length));
		this->_senderSSRC = htonl(senderSSRC);
	}

	[[nodiscard]] inline RTCP_ReportBlock *getReportBlock(int num) { return &_reportBlocks + num; }
	[[nodiscard]] inline const RTCP_ReportBlock *getReportBlock(int num) const {
		return &_reportBlocks + num;
	}

	[[nodiscard]] inline size_t getSize() const {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		return sizeof(uint32_t) * (1 + size_t(header.length()));
	}

	inline uint64_t ntpTimestamp() const { return ntohll(_ntpTimestamp); }
	inline uint32_t rtpTimestamp() const { return ntohl(_rtpTimestamp); }
	inline uint32_t packetCount() const { return ntohl(_packetCount); }
	inline uint32_t octetCount() const { return ntohl(_octetCount); }
	inline uint32_t senderSSRC() const { return ntohl(_senderSSRC); }

	inline void setNtpTimestamp(uint32_t ts) { _ntpTimestamp = htonll(ts); }
	inline void setRtpTimestamp(uint32_t ts) { _rtpTimestamp = htonl(ts); }

	inline void log() const {
		header.log();
		PLOG_VERBOSE << "RTCP SR: "
		             << " SSRC=" << senderSSRC() << ", NTP_TS=" << ntpTimestamp()
		             << ", RTP_TS=" << rtpTimestamp() << ", packetCount=" << packetCount()
		             << ", octetCount=" << octetCount();

		for (unsigned i = 0; i < unsigned(header.reportCount()); i++) {
			getReportBlock(i)->log();
		}
	}
};

struct RTCP_RR {
	RTCP_HEADER header;
	SSRC _senderSSRC;

private:
	RTCP_ReportBlock _reportBlocks;

public:
	[[nodiscard]] inline RTCP_ReportBlock *getReportBlock(int num) { return &_reportBlocks + num; }
	[[nodiscard]] inline const RTCP_ReportBlock *getReportBlock(int num) const {
		return &_reportBlocks + num;
	}

	inline SSRC senderSSRC() const { return ntohl(_senderSSRC); }
	inline void setSenderSSRC(SSRC ssrc) { this->_senderSSRC = htonl(ssrc); }

	[[nodiscard]] inline size_t getSize() const {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		return sizeof(uint32_t) * (1 + size_t(header.length()));
	}

	inline void preparePacket(SSRC senderSSRC, uint8_t reportCount) {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		size_t length = (sizeWithReportBlocks(reportCount) / 4) - 1;
		header.prepareHeader(201, reportCount, uint16_t(length));
		this->_senderSSRC = htonl(senderSSRC);
	}

	inline static size_t sizeWithReportBlocks(uint8_t reportCount) {
		return sizeof(header) + 4 + size_t(reportCount) * sizeof(RTCP_ReportBlock);
	}

	inline bool isSenderReport() { return header.payloadType() == 200; }

	inline bool isReceiverReport() { return header.payloadType() == 201; }

	inline void log() const {
		header.log();
		PLOG_VERBOSE << "RTCP RR: "
		             << " SSRC=" << ntohl(_senderSSRC);

		for (unsigned i = 0; i < unsigned(header.reportCount()); i++) {
			getReportBlock(i)->log();
		}
	}
};

struct RTCP_REMB {
	RTCP_FB_HEADER header;

	/*! \brief Unique identifier ('R' 'E' 'M' 'B') */
	char id[4];

	/*! \brief Num SSRC, Br Exp, Br Mantissa (bit mask) */
	uint32_t bitrate;

	SSRC ssrc[1];

	[[nodiscard]] unsigned int getSize() const {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		return sizeof(uint32_t) * (1 + header.header.length());
	}

	void preparePacket(SSRC senderSSRC, unsigned int numSSRC, unsigned int in_bitrate) {

		// Report Count becomes the format here.
		header.header.prepareHeader(206, 15, 0);

		// Always zero.
		header.setMediaSourceSSRC(0);

		header.setPacketSenderSSRC(senderSSRC);

		id[0] = 'R';
		id[1] = 'E';
		id[2] = 'M';
		id[3] = 'B';

		setBitrate(numSSRC, in_bitrate);
	}

	void setBitrate(unsigned int numSSRC, unsigned int in_bitrate) {
		unsigned int exp = 0;
		while (in_bitrate > pow(2, 18) - 1) {
			exp++;
			in_bitrate /= 2;
		}

		// "length" in packet is one less than the number of 32 bit words in the packet.
		header.header.setLength(
		    uint16_t((offsetof(RTCP_REMB, ssrc) / sizeof(uint32_t)) - 1 + numSSRC));

		this->bitrate = htonl((numSSRC << (32u - 8u)) | (exp << (32u - 8u - 6u)) | in_bitrate);
	}

	void setSsrc(int iterator, SSRC newSssrc) { ssrc[iterator] = htonl(newSssrc); }

	size_t static inline sizeWithSSRCs(int count) {
		return sizeof(RTCP_REMB) + (count - 1) * sizeof(SSRC);
	}
};

struct RTCP_PLI {
	RTCP_FB_HEADER header;

	void preparePacket(SSRC messageSSRC) {
		header.header.prepareHeader(206, 1, 2);
		header.setPacketSenderSSRC(messageSSRC);
		header.setMediaSourceSSRC(messageSSRC);
	}

	void print() { header.log(); }

	[[nodiscard]] static unsigned int size() { return sizeof(RTCP_FB_HEADER); }
};

struct RTCP_FIR_PART {
	uint32_t ssrc;
	uint8_t seqNo;
	uint8_t dummy1;
	uint16_t dummy2;
};

struct RTCP_FIR {
	RTCP_FB_HEADER header;
	RTCP_FIR_PART parts[1];

	void preparePacket(SSRC messageSSRC, uint8_t seqNo) {
		header.header.prepareHeader(206, 4, 2 + 2 * 1);
		header.setPacketSenderSSRC(messageSSRC);
		header.setMediaSourceSSRC(messageSSRC);
		parts[0].ssrc = htonl(messageSSRC);
		parts[0].seqNo = seqNo;
	}

	void print() { header.log(); }

	[[nodiscard]] static unsigned int size() {
		return sizeof(RTCP_FB_HEADER) + sizeof(RTCP_FIR_PART);
	}
};

struct RTCP_NACK_PART {
	uint16_t pid;
	uint16_t blp;
};

class RTCP_NACK {
public:
	RTCP_FB_HEADER header;
	RTCP_NACK_PART parts[1];

public:
	void preparePacket(SSRC ssrc, unsigned int discreteSeqNoCount) {
		header.header.prepareHeader(205, 1, 2 + uint16_t(discreteSeqNoCount));
		header.setMediaSourceSSRC(ssrc);
		header.setPacketSenderSSRC(ssrc);
	}

	/**
	 * Add a packet to the list of missing packets.
	 * @param fciCount The number of FCI fields that are present in this packet.
	 *                  Let the number start at zero and let this function grow the number.
	 * @param fciPID The seq no of the active FCI. It will be initialized automatically, and will
	 * change automatically.
	 * @param missingPacket The seq no of the missing packet. This will be added to the queue.
	 * @return true if the packet has grown, false otherwise.
	 */
	bool addMissingPacket(unsigned int *fciCount, uint16_t *fciPID, uint16_t missingPacket) {
		if (*fciCount == 0 || missingPacket < *fciPID || missingPacket > (*fciPID + 16)) {
			parts[*fciCount].pid = htons(missingPacket);
			parts[*fciCount].blp = 0;
			*fciPID = missingPacket;
			(*fciCount)++;
			return true;
		} else {
			// TODO SPEEED!
			auto blp = ntohs(parts[(*fciCount) - 1].blp);
			auto newBit = 1u << (unsigned int)(missingPacket - (1 + *fciPID));
			parts[(*fciCount) - 1].blp = htons(blp | newBit);
			return false;
		}
	}

	[[nodiscard]] static unsigned int getSize(unsigned int discreteSeqNoCount) {
		return offsetof(RTCP_NACK, parts) + sizeof(RTCP_NACK_PART) * discreteSeqNoCount;
	}

	[[nodiscard]] unsigned int getSeqNoCount() { return header.header.length() - 2; }
};

class RTP_RTX {
private:
	RTP header;

public:
	size_t copyTo(RTP *dest, size_t totalSize, uint8_t originalPayloadType) {
		memmove((char *)dest, (char *)this, header.getSize());
		dest->setSeqNumber(getOriginalSeqNo());
		dest->setPayloadType(originalPayloadType);
		memmove(dest->getBody(), getBody(), getBodySize(totalSize));
		return totalSize;
	}

	[[nodiscard]] uint16_t getOriginalSeqNo() const {
		return ntohs(*(uint16_t *)(header.getBody()));
	}

	[[nodiscard]] char *getBody() { return header.getBody() + sizeof(uint16_t); }

	[[nodiscard]] const char *getBody() const { return header.getBody() + sizeof(uint16_t); }

	[[nodiscard]] size_t getBodySize(size_t totalSize) {
		return totalSize - (getBody() - reinterpret_cast<char *>(this));
	}

	[[nodiscard]] RTP &getHeader() { return header; }

	size_t normalizePacket(size_t totalSize, SSRC originalSSRC, uint8_t originalPayloadType) {
		header.setSeqNumber(getOriginalSeqNo());
		header.setSsrc(originalSSRC);
		header.setPayloadType(originalPayloadType);
		// TODO, the -12 is the size of the header (which is variable!)
		memmove(header.getBody(), header.getBody() + sizeof(uint16_t),
		        totalSize - 12 - sizeof(uint16_t));
		return totalSize - sizeof(uint16_t);
	}
};

#pragma pack(pop)

}; // namespace rtc

#endif
