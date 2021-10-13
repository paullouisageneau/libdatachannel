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

#include "rtp.hpp"

#include "impl/internals.hpp"

#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifndef htonll
#define htonll(x)                                                                                  \
	((uint64_t)(((uint64_t)htonl((uint32_t)(x))) << 32) | (uint64_t)htonl((uint32_t)((x) >> 32)))
#endif
#ifndef ntohll
#define ntohll(x) htonll(x)
#endif

namespace rtc {

uint8_t RTP::version() const { return _first >> 6; }
bool RTP::padding() const { return (_first >> 5) & 0x01; }
bool RTP::extension() const { return (_first >> 4) & 0x01; }
uint8_t RTP::csrcCount() const { return _first & 0x0F; }
uint8_t RTP::marker() const { return _payloadType & 0b10000000; }
uint8_t RTP::payloadType() const { return _payloadType & 0b01111111; }
uint16_t RTP::seqNumber() const { return ntohs(_seqNumber); }
uint32_t RTP::timestamp() const { return ntohl(_timestamp); }
uint32_t RTP::ssrc() const { return ntohl(_ssrc); }

size_t RTP::getSize() const {
	return reinterpret_cast<const char *>(&_csrc) - reinterpret_cast<const char *>(this) +
	       sizeof(SSRC) * csrcCount();
}

size_t RTP::getExtensionHeaderSize() const {
	auto header = reinterpret_cast<const RTP_ExtensionHeader *>(getExtensionHeader());
	if (header) {
		return header->getSize() + sizeof(RTP_ExtensionHeader);
	}
	return 0;
}

const RTP_ExtensionHeader *RTP::getExtensionHeader() const {
	if (extension()) {
		auto header = reinterpret_cast<const char *>(&_csrc) + sizeof(SSRC) * csrcCount();
		return reinterpret_cast<const RTP_ExtensionHeader *>(header);
	}
	return nullptr;
}

RTP_ExtensionHeader *RTP::getExtensionHeader() {
	if (extension()) {
		auto header = reinterpret_cast<char *>(&_csrc) + sizeof(SSRC) * csrcCount();
		return reinterpret_cast<RTP_ExtensionHeader *>(header);
	}
	return nullptr;
}

const char *RTP::getBody() const {
	return reinterpret_cast<const char *>(&_csrc) + sizeof(SSRC) * csrcCount() + getExtensionHeaderSize();
}

char *RTP::getBody() { return reinterpret_cast<char *>(&_csrc) + sizeof(SSRC) * csrcCount() + getExtensionHeaderSize(); }

void RTP::preparePacket() { _first |= (1 << 7); }

void RTP::setSeqNumber(uint16_t newSeqNo) { _seqNumber = htons(newSeqNo); }

void RTP::setPayloadType(uint8_t newPayloadType) {
	_payloadType = (_payloadType & 0b10000000u) | (0b01111111u & newPayloadType);
}

void RTP::setSsrc(uint32_t in_ssrc) { _ssrc = htonl(in_ssrc); }

void RTP::setMarker(bool marker) { _payloadType = (_payloadType & 0x7F) | (marker << 7); };

void RTP::setTimestamp(uint32_t i) { _timestamp = htonl(i); }

void RTP::setExtension(bool extension) { _first = (_first & ~0x10) | ((extension & 1) << 4); }

void RTP::log() const {
	PLOG_VERBOSE << "RTP V: " << (int)version() << " P: " << (padding() ? "P" : " ")
	             << " X: " << (extension() ? "X" : " ") << " CC: " << (int)csrcCount()
	             << " M: " << (marker() ? "M" : " ") << " PT: " << (int)payloadType()
	             << " SEQNO: " << seqNumber() << " TS: " << timestamp();
}

uint16_t RTP_ExtensionHeader::profileSpecificId() const { return ntohs(_profileSpecificId); }

uint16_t RTP_ExtensionHeader::headerLength() const { return ntohs(_headerLength); }

size_t RTP_ExtensionHeader::getSize() const { return headerLength() * 4; }

const char *RTP_ExtensionHeader::getBody() const { return reinterpret_cast<const char *>((&_headerLength) + 1); }

char *RTP_ExtensionHeader::getBody() { return reinterpret_cast<char *>((&_headerLength) + 1); }

void RTP_ExtensionHeader::setProfileSpecificId(uint16_t profileSpecificId) {
	_profileSpecificId = htons(profileSpecificId);
}

void RTP_ExtensionHeader::setHeaderLength(uint16_t headerLength) {
	_headerLength = htons(headerLength);
}

void RTP_ExtensionHeader::clearBody() { std::memset(getBody(), 0, getSize()); }

void RTP_ExtensionHeader::writeCurrentVideoOrientation(size_t offset, uint8_t id, uint8_t value)
{
	if ((id == 0) || (id > 14) || ((offset + 2) > getSize())) return;
	auto buf = getBody() + offset;
	buf[0] = id << 4;
	buf[1] = value;
}

SSRC RTCP_ReportBlock::getSSRC() const { return ntohl(_ssrc); }

void RTCP_ReportBlock::preparePacket(SSRC in_ssrc, [[maybe_unused]] unsigned int packetsLost,
                                     [[maybe_unused]] unsigned int totalPackets,
                                     uint16_t highestSeqNo, uint16_t seqNoCycles, uint32_t jitter,
                                     uint64_t lastSR_NTP, uint64_t lastSR_DELAY) {
	setSeqNo(highestSeqNo, seqNoCycles);
	setJitter(jitter);
	setSSRC(in_ssrc);

	// Middle 32 bits of NTP Timestamp
	// _lastReport = lastSR_NTP >> 16u;
	setNTPOfSR(uint64_t(lastSR_NTP));
	setDelaySinceSR(uint32_t(lastSR_DELAY));

	// The delay, expressed in units of 1/65536 seconds
	// _delaySinceLastReport = lastSR_DELAY;
}

void RTCP_ReportBlock::setSSRC(SSRC in_ssrc) { _ssrc = htonl(in_ssrc); }

void RTCP_ReportBlock::setPacketsLost([[maybe_unused]] unsigned int packetsLost,
                                      [[maybe_unused]] unsigned int totalPackets) {
	// TODO Implement loss percentages.
	_fractionLostAndPacketsLost = 0;
}

unsigned int RTCP_ReportBlock::getLossPercentage() const {
	// TODO Implement loss percentages.
	return 0;
}

unsigned int RTCP_ReportBlock::getPacketLostCount() const {
	// TODO Implement total packets lost.
	return 0;
}

uint16_t RTCP_ReportBlock::seqNoCycles() const { return ntohs(_seqNoCycles); }

uint16_t RTCP_ReportBlock::highestSeqNo() const { return ntohs(_highestSeqNo); }

uint32_t RTCP_ReportBlock::jitter() const { return ntohl(_jitter); }

uint32_t RTCP_ReportBlock::delaySinceSR() const { return ntohl(_delaySinceLastReport); }

void RTCP_ReportBlock::setSeqNo(uint16_t highestSeqNo, uint16_t seqNoCycles) {
	_highestSeqNo = htons(highestSeqNo);
	_seqNoCycles = htons(seqNoCycles);
}

void RTCP_ReportBlock::setJitter(uint32_t jitter) { _jitter = htonl(jitter); }

void RTCP_ReportBlock::setNTPOfSR(uint64_t ntp) { _lastReport = htonll(ntp >> 16u); }

uint32_t RTCP_ReportBlock::getNTPOfSR() const { return ntohl(_lastReport) << 16u; }

void RTCP_ReportBlock::setDelaySinceSR(uint32_t sr) {
	// The delay, expressed in units of 1/65536 seconds
	_delaySinceLastReport = htonl(sr);
}

void RTCP_ReportBlock::log() const {
	PLOG_VERBOSE << "RTCP report block: "
	             << "ssrc="
	             << ntohl(_ssrc)
	             // TODO: Implement these reports
	             //	<< ", fractionLost=" << fractionLost
	             //	<< ", packetsLost=" << packetsLost
	             << ", highestSeqNo=" << highestSeqNo() << ", seqNoCycles=" << seqNoCycles()
	             << ", jitter=" << jitter() << ", lastSR=" << getNTPOfSR()
	             << ", lastSRDelay=" << delaySinceSR();
}

uint8_t RTCP_HEADER::version() const { return _first >> 6; }

bool RTCP_HEADER::padding() const { return (_first >> 5) & 0x01; }

uint8_t RTCP_HEADER::reportCount() const { return _first & 0x1F; }

uint8_t RTCP_HEADER::payloadType() const { return _payloadType; }

uint16_t RTCP_HEADER::length() const { return ntohs(_length); }

size_t RTCP_HEADER::lengthInBytes() const { return (1 + length()) * 4; }

void RTCP_HEADER::setPayloadType(uint8_t type) { _payloadType = type; }

void RTCP_HEADER::setReportCount(uint8_t count) {
	_first = (_first & 0b11100000u) | (count & 0b00011111u);
}

void RTCP_HEADER::setLength(uint16_t length) { _length = htons(length); }

void RTCP_HEADER::prepareHeader(uint8_t payloadType, uint8_t reportCount, uint16_t length) {
	_first = 0b10000000; // version 2, no padding
	setReportCount(reportCount);
	setPayloadType(payloadType);
	setLength(length);
}

void RTCP_HEADER::log() const {
	PLOG_VERBOSE << "RTCP header: "
	             << "version=" << unsigned(version()) << ", padding=" << padding()
	             << ", reportCount=" << unsigned(reportCount())
	             << ", payloadType=" << unsigned(payloadType()) << ", length=" << length();
}

SSRC RTCP_FB_HEADER::packetSenderSSRC() const { return ntohl(_packetSender); }

SSRC RTCP_FB_HEADER::mediaSourceSSRC() const { return ntohl(_mediaSource); }

void RTCP_FB_HEADER::setPacketSenderSSRC(SSRC ssrc) { _packetSender = htonl(ssrc); }

void RTCP_FB_HEADER::setMediaSourceSSRC(SSRC ssrc) { _mediaSource = htonl(ssrc); }

void RTCP_FB_HEADER::log() const {
	header.log();
	PLOG_VERBOSE << "FB: "
	             << " packet sender: " << packetSenderSSRC()
	             << " media source: " << mediaSourceSSRC();
}

unsigned int RTCP_SR::Size(unsigned int reportCount) {
	return sizeof(RTCP_HEADER) + 24 + reportCount * sizeof(RTCP_ReportBlock);
}

void RTCP_SR::preparePacket(SSRC senderSSRC, uint8_t reportCount) {
	unsigned int length = ((sizeof(header) + 24 + reportCount * sizeof(RTCP_ReportBlock)) / 4) - 1;
	header.prepareHeader(200, reportCount, uint16_t(length));
	this->_senderSSRC = htonl(senderSSRC);
}

const RTCP_ReportBlock *RTCP_SR::getReportBlock(int num) const { return &_reportBlocks + num; }

RTCP_ReportBlock *RTCP_SR::getReportBlock(int num) { return &_reportBlocks + num; }

size_t RTCP_SR::getSize() const {
	// "length" in packet is one less than the number of 32 bit words in the packet.
	return sizeof(uint32_t) * (1 + size_t(header.length()));
}

uint64_t RTCP_SR::ntpTimestamp() const { return ntohll(_ntpTimestamp); }
uint32_t RTCP_SR::rtpTimestamp() const { return ntohl(_rtpTimestamp); }
uint32_t RTCP_SR::packetCount() const { return ntohl(_packetCount); }
uint32_t RTCP_SR::octetCount() const { return ntohl(_octetCount); }
uint32_t RTCP_SR::senderSSRC() const { return ntohl(_senderSSRC); }

void RTCP_SR::setNtpTimestamp(uint64_t ts) { _ntpTimestamp = htonll(ts); }
void RTCP_SR::setRtpTimestamp(uint32_t ts) { _rtpTimestamp = htonl(ts); }
void RTCP_SR::setOctetCount(uint32_t ts) { _octetCount = htonl(ts); }
void RTCP_SR::setPacketCount(uint32_t ts) { _packetCount = htonl(ts); }

void RTCP_SR::log() const {
	header.log();
	PLOG_VERBOSE << "RTCP SR: "
	             << " SSRC=" << senderSSRC() << ", NTP_TS=" << ntpTimestamp()
	             << ", RTP_TS=" << rtpTimestamp() << ", packetCount=" << packetCount()
	             << ", octetCount=" << octetCount();

	for (unsigned i = 0; i < unsigned(header.reportCount()); i++) {
		getReportBlock(i)->log();
	}
}

unsigned int RTCP_SDES_ITEM::Size(uint8_t textLength) { return textLength + 2; }

std::string RTCP_SDES_ITEM::text() const { return std::string(_text, _length); }

void RTCP_SDES_ITEM::setText(std::string text) {
	if (text.size() > 0xFF)
		throw std::invalid_argument("text is too long");

	_length = uint8_t(text.size());
	memcpy(_text, text.data(), text.size());
}

uint8_t RTCP_SDES_ITEM::length() const { return _length; }

unsigned int RTCP_SDES_CHUNK::Size(const std::vector<uint8_t> textLengths) {
	unsigned int itemsSize = 0;
	for (auto length : textLengths) {
		itemsSize += RTCP_SDES_ITEM::Size(length);
	}
	auto nullTerminatedItemsSize = itemsSize + 1;
	auto words = uint8_t(std::ceil(double(nullTerminatedItemsSize) / 4)) + 1;
	return words * 4;
}

SSRC RTCP_SDES_CHUNK::ssrc() const { return ntohl(_ssrc); }

void RTCP_SDES_CHUNK::setSSRC(SSRC ssrc) { _ssrc = htonl(ssrc); }

const RTCP_SDES_ITEM *RTCP_SDES_CHUNK::getItem(int num) const {
	auto base = &_items;
	while (num-- > 0) {
		auto itemSize = RTCP_SDES_ITEM::Size(base->length());
		base = reinterpret_cast<const RTCP_SDES_ITEM *>(reinterpret_cast<const uint8_t *>(base) +
		                                                itemSize);
	}
	return reinterpret_cast<const RTCP_SDES_ITEM *>(base);
}

RTCP_SDES_ITEM *RTCP_SDES_CHUNK::getItem(int num) {
	auto base = &_items;
	while (num-- > 0) {
		auto itemSize = RTCP_SDES_ITEM::Size(base->length());
		base = reinterpret_cast<RTCP_SDES_ITEM *>(reinterpret_cast<uint8_t *>(base) + itemSize);
	}
	return reinterpret_cast<RTCP_SDES_ITEM *>(base);
}

unsigned int RTCP_SDES_CHUNK::getSize() const {
	std::vector<uint8_t> textLengths{};
	unsigned int i = 0;
	auto item = getItem(i);
	while (item->type != 0) {
		textLengths.push_back(item->length());
		item = getItem(++i);
	}
	return Size(textLengths);
}

long RTCP_SDES_CHUNK::safelyCountChunkSize(size_t maxChunkSize) const {
	if (maxChunkSize < RTCP_SDES_CHUNK::Size({})) {
		// chunk is truncated
		return -1;
	}

	size_t size = sizeof(SSRC);
	unsigned int i = 0;
	// We can always access first 4 bytes of first item (in case of no items there will be 4
	// null bytes)
	auto item = getItem(i);
	std::vector<uint8_t> textsLength{};
	while (item->type != 0) {
		if (size + RTCP_SDES_ITEM::Size(0) > maxChunkSize) {
			// item is too short
			return -1;
		}
		auto itemLength = item->length();
		if (size + RTCP_SDES_ITEM::Size(itemLength) >= maxChunkSize) {
			// item is too large (it can't be equal to chunk size because after item there
			// must be 1-4 null bytes as padding)
			return -1;
		}
		textsLength.push_back(itemLength);
		// safely to access next item
		item = getItem(++i);
	}
	auto realSize = RTCP_SDES_CHUNK::Size(textsLength);
	if (realSize > maxChunkSize) {
		// Chunk is too large
		return -1;
	}
	return realSize;
}

unsigned int RTCP_SDES::Size(const std::vector<std::vector<uint8_t>> lengths) {
	unsigned int chunks_size = 0;
	for (auto length : lengths)
		chunks_size += RTCP_SDES_CHUNK::Size(length);

	return 4 + chunks_size;
}

bool RTCP_SDES::isValid() const {
	auto chunksSize = header.lengthInBytes() - sizeof(header);
	if (chunksSize == 0) {
		return true;
	}
	// there is at least one chunk
	unsigned int i = 0;
	unsigned int size = 0;
	while (size < chunksSize) {
		if (chunksSize < size + RTCP_SDES_CHUNK::Size({})) {
			// chunk is truncated
			return false;
		}
		auto chunk = getChunk(i++);
		auto chunkSize = chunk->safelyCountChunkSize(chunksSize - size);
		if (chunkSize < 0) {
			// chunk is invalid
			return false;
		}
		size += chunkSize;
	}
	return size == chunksSize;
}

unsigned int RTCP_SDES::chunksCount() const {
	if (!isValid()) {
		return 0;
	}
	uint16_t chunksSize = 4 * (header.length() + 1) - sizeof(header);
	unsigned int size = 0;
	unsigned int i = 0;
	while (size < chunksSize) {
		size += getChunk(i++)->getSize();
	}
	return i;
}

const RTCP_SDES_CHUNK *RTCP_SDES::getChunk(int num) const {
	auto base = &_chunks;
	while (num-- > 0) {
		auto chunkSize = base->getSize();
		base = reinterpret_cast<const RTCP_SDES_CHUNK *>(reinterpret_cast<const uint8_t *>(base) +
		                                                 chunkSize);
	}
	return reinterpret_cast<const RTCP_SDES_CHUNK *>(base);
}

RTCP_SDES_CHUNK *RTCP_SDES::getChunk(int num) {
	auto base = &_chunks;
	while (num-- > 0) {
		auto chunkSize = base->getSize();
		base = reinterpret_cast<RTCP_SDES_CHUNK *>(reinterpret_cast<uint8_t *>(base) + chunkSize);
	}
	return reinterpret_cast<RTCP_SDES_CHUNK *>(base);
}

void RTCP_SDES::preparePacket(uint8_t chunkCount) {
	unsigned int chunkSize = 0;
	for (uint8_t i = 0; i < chunkCount; i++) {
		auto chunk = getChunk(i);
		chunkSize += chunk->getSize();
	}
	uint16_t length = uint16_t((sizeof(header) + chunkSize) / 4 - 1);
	header.prepareHeader(202, chunkCount, length);
}

const RTCP_ReportBlock *RTCP_RR::getReportBlock(int num) const { return &_reportBlocks + num; }

RTCP_ReportBlock *RTCP_RR::getReportBlock(int num) { return &_reportBlocks + num; }

size_t RTCP_RR::SizeWithReportBlocks(uint8_t reportCount) {
	return sizeof(header) + 4 + size_t(reportCount) * sizeof(RTCP_ReportBlock);
}

SSRC RTCP_RR::senderSSRC() const { return ntohl(_senderSSRC); }

bool RTCP_RR::isSenderReport() { return header.payloadType() == 200; }

bool RTCP_RR::isReceiverReport() { return header.payloadType() == 201; }

size_t RTCP_RR::getSize() const {
	// "length" in packet is one less than the number of 32 bit words in the packet.
	return sizeof(uint32_t) * (1 + size_t(header.length()));
}

void RTCP_RR::preparePacket(SSRC senderSSRC, uint8_t reportCount) {
	// "length" in packet is one less than the number of 32 bit words in the packet.
	size_t length = (SizeWithReportBlocks(reportCount) / 4) - 1;
	header.prepareHeader(201, reportCount, uint16_t(length));
	this->_senderSSRC = htonl(senderSSRC);
}

void RTCP_RR::setSenderSSRC(SSRC ssrc) { this->_senderSSRC = htonl(ssrc); }

void RTCP_RR::log() const {
	header.log();
	PLOG_VERBOSE << "RTCP RR: "
	             << " SSRC=" << ntohl(_senderSSRC);

	for (unsigned i = 0; i < unsigned(header.reportCount()); i++) {
		getReportBlock(i)->log();
	}
}

size_t RTCP_REMB::SizeWithSSRCs(int count) {
	return sizeof(RTCP_REMB) + (count - 1) * sizeof(SSRC);
}

unsigned int RTCP_REMB::getSize() const {
	// "length" in packet is one less than the number of 32 bit words in the packet.
	return sizeof(uint32_t) * (1 + header.header.length());
}

void RTCP_REMB::preparePacket(SSRC senderSSRC, unsigned int numSSRC, unsigned int in_bitrate) {

	// Report Count becomes the format here.
	header.header.prepareHeader(206, 15, 0);

	// Always zero.
	header.setMediaSourceSSRC(0);

	header.setPacketSenderSSRC(senderSSRC);

	_id[0] = 'R';
	_id[1] = 'E';
	_id[2] = 'M';
	_id[3] = 'B';

	setBitrate(numSSRC, in_bitrate);
}

void RTCP_REMB::setBitrate(unsigned int numSSRC, unsigned int in_bitrate) {
	unsigned int exp = 0;
	while (in_bitrate > pow(2, 18) - 1) {
		exp++;
		in_bitrate /= 2;
	}

	// "length" in packet is one less than the number of 32 bit words in the packet.
	header.header.setLength(
	    uint16_t((offsetof(RTCP_REMB, _ssrc) / sizeof(uint32_t)) - 1 + numSSRC));

	_bitrate = htonl((numSSRC << (32u - 8u)) | (exp << (32u - 8u - 6u)) | in_bitrate);
}

void RTCP_REMB::setSsrc(int iterator, SSRC newSssrc) { _ssrc[iterator] = htonl(newSssrc); }

unsigned int RTCP_PLI::Size() { return sizeof(RTCP_FB_HEADER); }

void RTCP_PLI::preparePacket(SSRC messageSSRC) {
	header.header.prepareHeader(206, 1, 2);
	header.setPacketSenderSSRC(messageSSRC);
	header.setMediaSourceSSRC(messageSSRC);
}

void RTCP_PLI::log() const { header.log(); }

unsigned int RTCP_FIR::Size() { return sizeof(RTCP_FB_HEADER) + sizeof(RTCP_FIR_PART); }

void RTCP_FIR::preparePacket(SSRC messageSSRC, uint8_t seqNo) {
	header.header.prepareHeader(206, 4, 2 + 2 * 1);
	header.setPacketSenderSSRC(messageSSRC);
	header.setMediaSourceSSRC(messageSSRC);
	parts[0].ssrc = htonl(messageSSRC);
	parts[0].seqNo = seqNo;
}

void RTCP_FIR::log() const { header.log(); }

uint16_t RTCP_NACK_PART::pid() { return ntohs(_pid); }
uint16_t RTCP_NACK_PART::blp() { return ntohs(_blp); }

void RTCP_NACK_PART::setPid(uint16_t pid) { _pid = htons(pid); }
void RTCP_NACK_PART::setBlp(uint16_t blp) { _blp = htons(blp); }

std::vector<uint16_t> RTCP_NACK_PART::getSequenceNumbers() {
	std::vector<uint16_t> result{};
	result.reserve(17);
	uint16_t p = pid();
	result.push_back(p);
	uint16_t bitmask = blp();
	uint16_t i = p + 1;
	while (bitmask > 0) {
		if (bitmask & 0x1) {
			result.push_back(i);
		}
		i += 1;
		bitmask >>= 1;
	}
	return result;
}

unsigned int RTCP_NACK::Size(unsigned int discreteSeqNoCount) {
	return offsetof(RTCP_NACK, parts) + sizeof(RTCP_NACK_PART) * discreteSeqNoCount;
}

unsigned int RTCP_NACK::getSeqNoCount() { return header.header.length() - 2; }

void RTCP_NACK::preparePacket(SSRC ssrc, unsigned int discreteSeqNoCount) {
	header.header.prepareHeader(205, 1, 2 + uint16_t(discreteSeqNoCount));
	header.setMediaSourceSSRC(ssrc);
	header.setPacketSenderSSRC(ssrc);
}

bool RTCP_NACK::addMissingPacket(unsigned int *fciCount, uint16_t *fciPID, uint16_t missingPacket) {
	if (*fciCount == 0 || missingPacket < *fciPID || missingPacket > (*fciPID + 16)) {
		parts[*fciCount].setPid(missingPacket);
		parts[*fciCount].setBlp(0);
		*fciPID = missingPacket;
		(*fciCount)++;
		return true;
	} else {
		// TODO SPEED!
		uint16_t blp = parts[(*fciCount) - 1].blp();
		uint16_t newBit = uint16_t(1u << (missingPacket - (1 + *fciPID)));
		parts[(*fciCount) - 1].setBlp(blp | newBit);
		return false;
	}
}

uint16_t RTP_RTX::getOriginalSeqNo() const { return ntohs(*(uint16_t *)(header.getBody())); }

const char *RTP_RTX::getBody() const { return header.getBody() + sizeof(uint16_t); }

char *RTP_RTX::getBody() { return header.getBody() + sizeof(uint16_t); }

size_t RTP_RTX::getBodySize(size_t totalSize) const {
	return totalSize - (getBody() - reinterpret_cast<const char *>(this));
}

size_t RTP_RTX::getSize() const { return header.getSize() + sizeof(uint16_t); }

size_t RTP_RTX::normalizePacket(size_t totalSize, SSRC originalSSRC, uint8_t originalPayloadType) {
	header.setSeqNumber(getOriginalSeqNo());
	header.setSsrc(originalSSRC);
	header.setPayloadType(originalPayloadType);
	// TODO, the -12 is the size of the header (which is variable!)
	memmove(header.getBody(), getBody(), totalSize - getSize());
	return totalSize - 2;
}

size_t RTP_RTX::copyTo(RTP *dest, size_t totalSize, uint8_t originalPayloadType) {
	memmove((char *)dest, (char *)this, header.getSize());
	dest->setSeqNumber(getOriginalSeqNo());
	dest->setPayloadType(originalPayloadType);
	memmove(dest->getBody(), getBody(), getBodySize(totalSize));
	return totalSize;
}

}; // namespace rtc
