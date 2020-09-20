/**
 * Copyright (c) 2020 Staz M
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

#include "rtcp.hpp"

#include <cmath>
#include <iostream>
#include <utility>

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

#pragma pack(push, 1)

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
	void print() {
		std::cout << " ssrc:" << ntohl(ssrc) <<
		    // TODO Implement these reports
		    //			  " fractionLost: " << fractionLost <<
		    //			  " packetsLost: " << packetsLost <<
		    " highestSeqNo:" << highestSeqNo() << " seqNoCycles:" << seqNoCycles()
		          << " jitter:" << jitter() << " lastSR:" << getNTPOfSR()
		          << " lastSRDelay:" << getDelaySinceSR();
	}

	void preparePacket(SSRC ssrc, [[maybe_unused]] unsigned int packetsLost,
	                   [[maybe_unused]] unsigned int totalPackets, uint16_t highestSeqNo,
	                   uint16_t seqNoCycles, uint32_t jitter, uint64_t lastSR_NTP,
	                   uint64_t lastSR_DELAY) {
		setSeqNo(highestSeqNo, seqNoCycles);
		setJitter(jitter);
		setSSRC(ssrc);

		// Middle 32 bits of NTP Timestamp
		//		  this->lastReport = lastSR_NTP >> 16u;
		setNTPOfSR(uint32_t(lastSR_NTP));
		setDelaySinceSR(uint32_t(lastSR_DELAY));

		// The delay, expressed in units of 1/65536 seconds
		//		  this->delaySinceLastReport = lastSR_DELAY;
	}

	void inline setSSRC(SSRC ssrc) { this->ssrc = htonl(ssrc); }
	SSRC inline getSSRC() const { return ntohl(ssrc); }

	void inline setPacketsLost([[maybe_unused]] unsigned int packetsLost,
	                           [[maybe_unused]] unsigned int totalPackets) {
		// TODO Implement loss percentages.
		_fractionLostAndPacketsLost = 0;
	}
	unsigned int inline getLossPercentage() const {
		// TODO Implement loss percentages.
		return 0;
	}
	unsigned int inline getPacketLostCount() const {
		// TODO Implement total packets lost.
		return 0;
	}

	uint16_t inline seqNoCycles() const { return ntohs(_seqNoCycles); }
	uint16_t inline highestSeqNo() const { return ntohs(_highestSeqNo); }
	uint32_t inline jitter() const { return ntohl(_jitter); }

	void inline setSeqNo(uint16_t highestSeqNo, uint16_t seqNoCycles) {
		_highestSeqNo = htons(highestSeqNo);
		_seqNoCycles = htons(seqNoCycles);
	}

	void inline setJitter(uint32_t jitter) { _jitter = htonl(jitter); }

	void inline setNTPOfSR(uint32_t ntp) { _lastReport = htonl(ntp >> 16u); }
	inline uint32_t getNTPOfSR() const { return ntohl(_lastReport) << 16u; }

	inline void setDelaySinceSR(uint32_t sr) {
		// The delay, expressed in units of 1/65536 seconds
		_delaySinceLastReport = htonl(sr);
	}
	inline uint32_t getDelaySinceSR() const { return ntohl(_delaySinceLastReport); }
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

	inline void setPayloadType(uint8_t type) { _payloadType = type; }
	inline void setReportCount(uint8_t count) { _first = (_first & 0xF0) | (count & 0x0F); }
	inline void setLength(uint16_t length) { _length = htons(length); }

	void prepareHeader(uint8_t payloadType, uint8_t reportCount, uint16_t length) {
		_first = 0x02 << 6; // version 2, no padding
		setReportCount(reportCount);
		setPayloadType(payloadType);
		setLength(length);
	}

	void print() {
		std::cout << "version:" << unsigned(version()) << " padding:" << (padding() ? "T" : "F")
		          << " reportCount: " << unsigned(reportCount())
		          << " payloadType:" << unsigned(payloadType()) << " length: " << length();
	}
};

struct RTCP_SR {
	RTCP_HEADER header;
	SSRC senderSSRC;

private:
	uint64_t _ntpTimestamp;
	uint32_t _rtpTimestamp;
	uint32_t _packetCount;
	uint32_t _octetCount;

	RTCP_ReportBlock _reportBlocks;

public:
	void print() {
		std::cout << "SR ";
		header.print();
		std::cout << " SSRC:" << ntohl(senderSSRC) << " NTP TS: " << ntpTimestamp()
		          << " RTP TS: " << rtpTimestamp() << " packetCount: " << packetCount()
		          << " octetCount: " << octetCount() << std::endl;

		for (unsigned i = 0; i < unsigned(header.reportCount()); i++) {
			getReportBlock(i)->print();
			std::cout << std::endl;
		}
	}

	inline void preparePacket(SSRC senderSSRC, uint8_t reportCount) {
		unsigned int length =
		    ((sizeof(header) + 24 + reportCount * sizeof(RTCP_ReportBlock)) / 4) - 1;
		header.prepareHeader(200, reportCount, uint16_t(length));
		this->senderSSRC = htonl(senderSSRC);
	}

	RTCP_ReportBlock *getReportBlock(int num) { return &_reportBlocks + num; }

	[[nodiscard]] size_t getSize() const {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		return sizeof(uint32_t) * (1 + size_t(header.length()));
	}

	inline uint32_t ntpTimestamp() const { return ntohll(_ntpTimestamp); }
	inline uint32_t rtpTimestamp() const { return ntohl(_rtpTimestamp); }
	inline uint32_t packetCount() const { return ntohl(_packetCount); }
	inline uint32_t octetCount() const { return ntohl(_octetCount); }

	inline void setNtpTimestamp(uint32_t ts) { _ntpTimestamp = htonll(ts); }
	inline void setRtpTimestamp(uint32_t ts) { _rtpTimestamp = htonl(ts); }
};

struct RTCP_RR {
	RTCP_HEADER header;
	SSRC senderSSRC;

private:
	RTCP_ReportBlock _reportBlocks;

public:
	void print() {
		std::cout << "RR ";
		header.print();
		std::cout << " SSRC:" << ntohl(senderSSRC) << std::endl;

		for (unsigned i = 0; i < unsigned(header.reportCount()); i++) {
			getReportBlock(i)->print();
			std::cout << std::endl;
		}
	}
	RTCP_ReportBlock *getReportBlock(int num) { return &_reportBlocks + num; }

	inline SSRC getSenderSSRC() const { return ntohl(senderSSRC); }
	inline void setSenderSSRC(SSRC ssrc) { this->senderSSRC = htonl(ssrc); }

	[[nodiscard]] inline size_t getSize() const {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		return sizeof(uint32_t) * (1 + size_t(header.length()));
	}

	inline void preparePacket(SSRC senderSSRC, uint8_t reportCount) {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		size_t length = (sizeWithReportBlocks(reportCount) / 4) - 1;
		header.prepareHeader(201, reportCount, uint16_t(length));
		this->senderSSRC = htonl(senderSSRC);
	}

	inline static size_t sizeWithReportBlocks(uint8_t reportCount) {
		return sizeof(header) + 4 + size_t(reportCount) * sizeof(RTCP_ReportBlock);
	}
};

struct RTP
{
private:
	uint8_t _first;
	uint8_t _payloadType;
	uint16_t _seqNumber;
	uint32_t _timestamp;

public:
	SSRC ssrc;
	SSRC csrc[16];

	inline uint8_t version() const { return _first >> 6; }
	inline bool padding() const { return (_first >> 5) & 0x01; }
	inline uint8_t csrcCount() const { return _first & 0x0F; }
	inline uint8_t payloadType() const { return _payloadType; }
	inline uint16_t seqNumber() const { return ntohs(_seqNumber); }
	inline uint32_t timestamp() const { return ntohl(_timestamp); }
};

struct RTCP_REMB {
	RTCP_HEADER header;

	SSRC senderSSRC;
	SSRC mediaSourceSSRC;

	// Unique identifier
	const char id[4] = {'R', 'E', 'M', 'B'};

	// Num SSRC, Br Exp, Br Mantissa (bit mask)
	uint32_t bitrate;

	SSRC ssrc[1];

	[[nodiscard]] size_t getSize() const {
		// "length" in packet is one less than the number of 32 bit words in the packet.
		return sizeof(uint32_t) * (1 + size_t(header.length()));
	}

	void preparePacket(SSRC senderSSRC, unsigned int numSSRC, unsigned int bitrate) {
		// Report Count becomes the format here.
		header.prepareHeader(206, 15, 0);

		// Always zero.
		mediaSourceSSRC = 0;

		this->senderSSRC = htonl(senderSSRC);
		setBitrate(numSSRC, bitrate);
	}

	void setBitrate(unsigned int numSSRC, unsigned int bitrate) {
		unsigned int exp = 0;
		while (bitrate > pow(2, 18) - 1) {
			exp++;
			bitrate /= 2;
		}

		// "length" in packet is one less than the number of 32 bit words in the packet.
		header.setLength(uint16_t((offsetof(RTCP_REMB, ssrc) / 4) - 1 + numSSRC));

		this->bitrate = htonl((numSSRC << (32u - 8u)) | (exp << (32u - 8u - 6u)) | bitrate);
	}

	// TODO Make this work
	//	  uint64_t getBitrate() const{
	//		  uint32_t ntohed = ntohl(this->bitrate);
	//		  uint64_t bitrate = ntohed & (unsigned int)(pow(2, 18)-1);
	//		  unsigned int exp = ntohed & ((unsigned int)( (pow(2, 6)-1)) << (32u-8u-6u));
	//		  return bitrate * pow(2,exp);
	//	  }
	//
	//	  uint8_t getNumSSRCS() const {
	//		  return ntohl(this->bitrate) & (((unsigned int) pow(2,8)-1) << (32u-8u));
	//	  }

	void print() {
		std::cout << "REMB ";
		header.print();
		std::cout << " SSRC:" << ntohl(senderSSRC);
	}

	void setSSRC(uint8_t iterator, SSRC ssrc) { this->ssrc[iterator] = htonl(ssrc); }

	static unsigned int sizeWithSSRCs(int numSSRC) {
		return (offsetof(RTCP_REMB, ssrc)) + sizeof(SSRC) * numSSRC;
	}
};

#pragma pack(pop)

void RtcpSession::onOutgoing(std::function<void(rtc::message_ptr)> cb) { mTxCallback = cb; }

std::optional<rtc::message_ptr> RtcpSession::incoming(rtc::message_ptr ptr) {
	if (ptr->type == rtc::Message::Type::Binary) {
		RTP *rtp = (RTP *)ptr->data();

		// https://tools.ietf.org/html/rfc3550#appendix-A.1
		if (rtp->version() != 2) {
			PLOG_WARNING << "RTP packet is not version 2";

			return std::nullopt;
		}
		if (rtp->payloadType() == 201 || rtp->payloadType() == 200) {
			PLOG_WARNING << "RTP packet has a payload type indicating RR/SR";

			return std::nullopt;
		}

		// TODO Implement the padding bit
		if (rtp->padding()) {
			PLOG_WARNING << "Padding processing not implemented";
		}

		mSsrc = ntohl(rtp->ssrc);

		uint32_t seqNo = rtp->seqNumber();
		// uint32_t rtpTS = rtp->getTS();

		if (mGreatestSeqNo < seqNo)
			mGreatestSeqNo = seqNo;

		return ptr;
	}

	assert(ptr->type == rtc::Message::Type::Control);
	auto rr = (RTCP_RR *)ptr->data();
	if (rr->header.payloadType() == 201) {
		// RR
		mSsrc = rr->getSenderSSRC();
		rr->print();
		std::cout << std::endl;
	} else if (rr->header.payloadType() == 200) {
		// SR
		mSsrc = rr->getSenderSSRC();
		auto sr = (RTCP_SR *)ptr->data();
		mSyncRTPTS = sr->rtpTimestamp();
		mSyncNTPTS = sr->ntpTimestamp();
		sr->print();
		std::cout << std::endl;

		// TODO For the time being, we will send RR's/REMB's when we get an SR
		pushRR(0);
		if (mRequestedBitrate > 0)
			pushREMB(mRequestedBitrate);
	}
	return std::nullopt;
}

void RtcpSession::requestBitrate(unsigned int newBitrate) {
	mRequestedBitrate = newBitrate;

	PLOG_DEBUG << "[GOOG-REMB] Requesting bitrate: " << newBitrate << std::endl;
	pushREMB(newBitrate);
}

void RtcpSession::pushREMB(unsigned int bitrate) {
	rtc::message_ptr msg =
	    rtc::make_message(RTCP_REMB::sizeWithSSRCs(1), rtc::Message::Type::Control);
	auto remb = (RTCP_REMB *)msg->data();
	remb->preparePacket(mSsrc, 1, bitrate);
	remb->setSSRC(0, mSsrc);
	remb->print();
	std::cout << std::endl;

	tx(msg);
}

void RtcpSession::pushRR(unsigned int lastSR_delay) {
	// std::cout << "size " << RTCP_RR::sizeWithReportBlocks(1) << std::endl;
	auto msg = rtc::make_message(RTCP_RR::sizeWithReportBlocks(1), rtc::Message::Type::Control);
	auto rr = (RTCP_RR *)msg->data();
	rr->preparePacket(mSsrc, 1);
	rr->getReportBlock(0)->preparePacket(mSsrc, 0, 0, mGreatestSeqNo, 0, 0, mSyncNTPTS,
	                                     lastSR_delay);
	rr->print();
	std::cout << std::endl;

	tx(msg);
}

void RtcpSession::tx(message_ptr msg) {
	try {
		mTxCallback(msg);
	} catch (const std::exception &e) {
		LOG_DEBUG << "RTCP tx failed: " << e.what();
	}
}

} // namespace rtc

