/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframeutility.hpp"

#if RTC_ENABLE_MEDIA

#include "rtp.hpp"

#include "impl/logcounter.hpp"
#include "impl/sframecodec.hpp"
#include "impl/sframecrypto.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rtc::impl {

static LogCounter COUNTER_SFRAME_DECODE_FAILED(plog::warning,
                                               "Number of SFrame frames that failed to decode");

binary sframe::MakeAAD(const binary &header, const binary &metadata) {
	binary aad;
	aad.reserve(header.size() + metadata.size());
	aad.insert(aad.end(), header.begin(), header.end());
	aad.insert(aad.end(), metadata.begin(), metadata.end());
	return aad;
}

SFrameKeyMaterial sframe::GetSFrameKeys(uint64_t kid, uint64_t ctr, const binary &baseKey,
                                        uint16_t cipherSuiteId) {
	SFrameKidKeys keys = sframe::DeriveKeysForKid(baseKey, kid, cipherSuiteId);

	size_t nonceSize = sframe_crypto::GetNonceSize(cipherSuiteId);
	binary nonce = sframe::DeriveNonce(keys.salt, ctr, nonceSize);

	// The salt has done its job once the nonce is derived, and the key is moved rather than copied,
	// so this leaves one set of key bytes for the caller to account for rather than two here.
	sframe::Cleanse(keys.salt);
	return SFrameKeyMaterial{std::move(keys.encryptionKey), std::move(nonce)};
}

// RFC 9605 Section 4.4.2: the label is a fixed prefix, then the KID and the cipher suite as
// big-endian fields. The key and salt labels differ only in that prefix.
static binary makeSFrameLabel(const char *prefix, size_t prefixLen, uint64_t kid,
                              uint16_t cipherSuiteId) {
	binary label;
	label.insert(label.end(), reinterpret_cast<const std::byte *>(prefix),
	             reinterpret_cast<const std::byte *>(prefix) + prefixLen);

	auto kidBytes = sframe::EncodeBigEndian(kid, 8);
	label.insert(label.end(), kidBytes.begin(), kidBytes.end());

	auto cipherSuiteBytes = sframe::EncodeBigEndian(cipherSuiteId, 2);
	label.insert(label.end(), cipherSuiteBytes.begin(), cipherSuiteBytes.end());
	return label;
}

binary sframe::GetSFrameKeyLabel(uint64_t kid, uint16_t cipherSuiteId) {
	static constexpr char PREFIX[] = "SFrame 1.0 Secret key ";
	return makeSFrameLabel(PREFIX, sizeof(PREFIX) - 1, kid, cipherSuiteId);
}

binary sframe::GetSFrameSaltLabel(uint64_t kid, uint16_t cipherSuiteId) {
	static constexpr char PREFIX[] = "SFrame 1.0 Secret salt ";
	return makeSFrameLabel(PREFIX, sizeof(PREFIX) - 1, kid, cipherSuiteId);
}

binary sframe::DeriveNonce(const binary &sframeSalt, uint64_t ctr, size_t nonceSize) {
	// Encode ctr as big-endian, right-aligned in nonce_size bytes
	binary ctrBytes(nonceSize, std::byte{0});
	for (size_t i = 0; i < sizeof(ctr) && i < nonceSize; ++i) {
		ctrBytes[nonceSize - 1 - i] = static_cast<std::byte>((ctr >> (8 * i)) & 0xFF);
	}

	// The salt is derived at the cipher suite's nonce size, but this is exported: a
	// caller passing a shorter salt would read past its end.
	if (sframeSalt.size() < nonceSize)
		throw std::invalid_argument("SFrame salt is shorter than the nonce size");

	binary nonce(nonceSize);
	for (size_t i = 0; i < nonceSize; ++i) {
		nonce[i] = sframeSalt[i] ^ ctrBytes[i];
	}

	return nonce;
}

binary sframe::EncodePlaintext(uint16_t cipherSuiteId, const binary &sframeKey, const binary &nonce,
                               const binary &aad, const binary &plaintext) {
	return sframe_crypto::EncodePlaintext(cipherSuiteId, sframeKey, nonce, aad, plaintext);
}

binary sframe::DecodeCiphertext(uint16_t cipherSuiteId, const binary &sframeKey,
                                const binary &nonce, const binary &aad, const binary &cipherText) {
	return sframe_crypto::DecodeCiphertext(cipherSuiteId, sframeKey, nonce, aad, cipherText);
}

size_t sframe::MinimalByteLength(uint64_t v) {
	for (int i = 7; i >= 0; --i) {
		if ((v >> (8 * i)) != 0) {
			return i + 1;
		}
	}
	return 1;
}

binary sframe::EncodeBigEndian(uint64_t value, size_t numBytes) {
	binary out(numBytes);
	for (size_t i = 0; i < numBytes; ++i) {
		out[numBytes - 1 - i] = std::byte((value >> (8 * i)) & 0xFF);
	}
	return out;
}

uint64_t sframe::DecodeBigEndian(const binary &buf, size_t offset, size_t len) {
	if (len == 0 || len > 8) {
		throw std::invalid_argument("Invalid big-endian length");
	}

	if (offset + len > buf.size()) {
		throw std::out_of_range("Buffer too small for decode");
	}
	uint64_t value = 0;
	for (size_t i = 0; i < len; ++i) {
		value = (value << 8) | std::to_integer<uint8_t>(buf[offset + i]);
	}
	return value;
}

static uint64_t ratchetMask(uint8_t ratchetStepBits) {
	if (ratchetStepBits == 0)
		return 0;
	return (uint64_t(1) << ratchetStepBits) - 1;
}

uint64_t sframe::MakeKid(uint64_t keyGeneration, uint64_t ratchetStep, uint8_t ratchetStepBits) {
	if (ratchetStepBits > SFrameMaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameMaxRatchetStepBits));

	// The generation is shifted up by R, so anything that does not fit in the remaining bits
	// would wrap and alias another generation -- and the KID is an HKDF label, so two
	// generations sharing a KID share a key.
	if (ratchetStepBits > 0 && keyGeneration >= (uint64_t(1) << (64 - ratchetStepBits)))
		throw std::invalid_argument("SFrame key generation does not fit in 64 bits alongside " +
		                            std::to_string(ratchetStepBits) + " ratchet step bits");

	const uint64_t mask = ratchetMask(ratchetStepBits);
	return (keyGeneration << ratchetStepBits) | (ratchetStep & mask);
}

uint64_t sframe::KeyGenerationFromKid(uint64_t kid, uint8_t ratchetStepBits) {
	if (ratchetStepBits > SFrameMaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameMaxRatchetStepBits));

	return kid >> ratchetStepBits;
}

uint64_t sframe::RatchetStepFromKid(uint64_t kid, uint8_t ratchetStepBits) {
	if (ratchetStepBits > SFrameMaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameMaxRatchetStepBits));

	return kid & ratchetMask(ratchetStepBits);
}

uint64_t sframe::NextRatchetKid(uint64_t kid, uint8_t ratchetStepBits) {
	if (ratchetStepBits > SFrameMaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(SFrameMaxRatchetStepBits));

	// ratchet_step % (1 << R): the step wraps to zero and the key generation above it is
	// left alone, so the ratchet never leaves the KID range the application allocated.
	const uint64_t mask = ratchetMask(ratchetStepBits);
	return (kid & ~mask) | ((kid + 1) & mask);
}

size_t sframe::MinBaseKeySize() { return sframe_crypto::MinBaseKeySize; }

bool sframe::ConstantTimeEquals(const binary &a, const binary &b) {
	return sframe_crypto::ConstantTimeEquals(a, b);
}

void sframe::Cleanse(binary &buffer) { sframe_crypto::Cleanse(buffer); }

void sframe::ValidateCipherSuite(uint16_t cipherSuiteId) {
	sframe_crypto::ValidateCipherSuite(cipherSuiteId);
}

void sframe::ValidateBaseKey(const binary &baseKey) { sframe_crypto::ValidateBaseKey(baseKey); }

binary sframe::DeriveSSRCKey(SSRC ssrc, const binary &baseKey, uint16_t cipherSuiteId) {
	return sframe_crypto::DeriveSSRCKey(ssrc, baseKey, cipherSuiteId);
}

bool sframe::IsValidDescriptor(uint8_t descriptor, bool isFirst, bool isLast) {
	// S and E must match the packet's position and T must be 0, since a packetized-origin
	// object is declined rather than mis-delivered. The 5 reserved bits are masked off rather
	// than required to be zero: they are sent as zero, but a future extension that sets one has
	// to remain decodable here.
	const uint8_t defined = SFrameDescriptorS | SFrameDescriptorE | SFrameDescriptorT;
	const uint8_t expected =
	    uint8_t((isFirst ? SFrameDescriptorS : 0) | (isLast ? SFrameDescriptorE : 0));
	return (descriptor & defined) == expected;
}

bool sframe::IsPacketizedOrigin(uint8_t descriptor) {
	return (descriptor & SFrameDescriptorT) != 0;
}

bool sframe::ParseRtpPayload(const binary &packet, size_t &hdrSize, size_t &payloadEnd) {
	// Need at least the fixed RTP header.
	if (packet.size() < sizeof(RtpHeader))
		return false;

	auto pkt = reinterpret_cast<const RtpHeader *>(packet.data());

	// getSize() is the fixed header plus the CSRC list (12 + csrcCount * 4).
	size_t headerSize = pkt->getSize();
	if (packet.size() < headerSize)
		return false; // truncated CSRC list

	if (pkt->extension()) {
		// Ensure the 4-byte extension header prefix is present before reading
		// its declared length, then that the full extension body is present.
		if (packet.size() < headerSize + sizeof(RtpExtensionHeader))
			return false; // truncated extension header
		headerSize += pkt->getExtensionHeaderSize();
		if (packet.size() < headerSize)
			return false; // truncated extension body
	}

	// Trailing RTP padding: when the P bit is set the last byte gives the number
	// of padding bytes (including itself, RFC 3550 5.1); remove them.
	size_t contentEnd = packet.size();
	if (pkt->padding()) {
		uint8_t pad = std::to_integer<uint8_t>(packet[packet.size() - 1]);
		if (pad == 0 || pad > packet.size() - headerSize)
			return false; // invalid padding count
		contentEnd = packet.size() - pad;
	}

	hdrSize = headerSize;
	payloadEnd = contentEnd;
	return true;
}

bool sframe::ParseSFramePacket(const shared_ptr<Message> &msg, size_t &hdrSize,
                               size_t &payloadEnd) {
	if (!ParseRtpPayload(*msg, hdrSize, payloadEnd))
		return false;

	// Must carry the 1-byte S/E/T descriptor plus at least one payload byte
	// (after removing any padding).
	return payloadEnd >= hdrSize + 2;
}

bool sframe::HasNoRtpPayload(const binary &packet) {
	size_t hdrSize = 0, payloadEnd = 0;
	return ParseRtpPayload(packet, hdrSize, payloadEnd) && payloadEnd == hdrSize;
}

SFrameKidKeys sframe::DeriveKeysForKid(const binary &baseKey, uint64_t kid,
                                       uint16_t cipherSuiteId) {
	SFrameDerivedKeys derived =
	    sframe_crypto::DeriveSFrameKeys(baseKey, cipherSuiteId,
	                                    sframe::GetSFrameKeyLabel(kid, cipherSuiteId),
	                                    sframe::GetSFrameSaltLabel(kid, cipherSuiteId));
	return {std::move(derived.contentEncryptionKey), std::move(derived.sframeSalt)};
}

binary sframe::RatchetKey(uint16_t cipherSuiteId, const binary &key) {
	return sframe_crypto::RatchetKey(cipherSuiteId, key);
}

void sframe::DecryptMessages(message_vector &messages, const std::vector<SSRC> &owners,
                             SFrameDecoderSet &decoders) {
	// One frame at a time, so reassembly order survives. Only reached when a batch carried several
	// SSRCs, which no in-tree caller produces.
	message_vector out;
	out.reserve(messages.size());
	for (size_t i = 0; i < messages.size(); ++i) {
		// Control never decrypts, and must not reach the call below: it takes a decoder up front,
		// so a placeholder SSRC would create an entry for a stream that does not exist.
		if (messages[i]->type == Message::Control) {
			out.push_back(std::move(messages[i]));
			continue;
		}

		message_vector one{std::move(messages[i])};
		DecryptMessages(one, i < owners.size() ? owners[i] : 0, decoders);
		for (auto &m : one)
			out.push_back(std::move(m));
	}
	messages.swap(out);
}

void sframe::DecryptMessages(message_vector &messages, SSRC ssrc, SFrameDecoderSet &decoders) {
	// No provider argument: the set holds one and refuses to be constructed without it, so there
	// is no null to check here.

	// One decoder per stream, with the catch-up allowance shared across the set; see
	// SFrameDecoderSet.
	SFrameDecoder &decoder = decoders.forSsrc(ssrc);

	message_vector decrypted;
	decrypted.reserve(messages.size());
	for (auto &msg : messages) {
		if (msg->type == Message::Control) {
			decrypted.push_back(std::move(msg));
			continue;
		}
		binary metadata;
		try {
			auto decrypted_msg = decoder.decodeFrame(msg, metadata);
			if (decrypted_msg && msg->frameInfo)
				decrypted_msg->frameInfo = msg->frameInfo;
			decrypted.push_back(std::move(decrypted_msg));
		} catch (const std::exception &) {
			// Rate limited: a peer can make this fire on every frame, and an unbounded
			// warning per failure is its own denial of service.
			COUNTER_SFRAME_DECODE_FAILED++;
		} catch (...) {
			COUNTER_SFRAME_DECODE_FAILED++;
		}
	}
	messages.swap(decrypted);
}

// RFC 9605 Section 4.3 header:
//
//   0 1 2 3 4 5 6 7
//  +-+-+-+-+-+-+-+-+---------------------------+---------------------------+
//  |X|  K  |Y|  C  |   KID... (X: 1..8 bytes)  |   CTR... (Y: 1..8 bytes)  |
//  +-+-+-+-+-+-+-+-+---------------------------+---------------------------+
//
// X and Y are the extended flags. Clear, K and C hold the KID and CTR values themselves
// (0..7) and no further bytes follow; set, they hold length-1 of the big-endian field that
// follows.
binary sframe::header::Encode(SFrameHeaderInfo headerInfo) {
	bool useBaseKidHeader = (headerInfo.kid < 8);
	bool useBaseCTRHeader = (headerInfo.ctr < 8);

	size_t kidLen = useBaseKidHeader ? 1 : sframe::MinimalByteLength(headerInfo.kid);
	size_t ctrLen = useBaseCTRHeader ? 1 : sframe::MinimalByteLength(headerInfo.ctr);

	uint8_t config = 0;
	if (!useBaseKidHeader)
		config |= 0x80; // X
	if (!useBaseCTRHeader)
		config |= 0x08; // Y

	uint8_t kidField = uint8_t(useBaseKidHeader ? headerInfo.kid : (kidLen - 1)) & 0x07;
	config |= uint8_t(kidField << 4);

	uint8_t ctrField = uint8_t(useBaseCTRHeader ? headerInfo.ctr : (ctrLen - 1)) & 0x07;
	config |= ctrField;

	binary header;
	header.push_back(std::byte(config));

	if (!useBaseKidHeader) {
		auto kidBytes = sframe::EncodeBigEndian(headerInfo.kid, kidLen);
		header.insert(header.end(), kidBytes.begin(), kidBytes.end());
	}

	if (!useBaseCTRHeader) {
		auto ctrBytes = sframe::EncodeBigEndian(headerInfo.ctr, ctrLen);
		header.insert(header.end(), ctrBytes.begin(), ctrBytes.end());
	}

	return header;
}

// Inverse of Encode(); see the layout above.
SFrameHeaderInfo sframe::header::Decode(const binary &encodedFrame) {
	// Exported and reachable with caller-supplied bytes, so the config byte is not assumed
	// to exist. DecodeBigEndian bounds-checks the extended KID/CTR fields below.
	if (encodedFrame.empty())
		throw std::invalid_argument("SFrame header is empty");

	uint8_t config = std::to_integer<uint8_t>(encodedFrame[0]);
	bool extendedKidFlag = (config & 0x80) != 0;
	uint8_t kidField = (config >> 4) & 0x07;
	bool extendedCTRFlag = (config & 0x08) != 0;
	uint8_t ctrField = config & 0x07;

	size_t offset = 1;
	uint64_t kid, ctr;

	if (!extendedKidFlag) {
		kid = kidField;
	} else {
		size_t kidLen = size_t(kidField) + 1;
		kid = sframe::DecodeBigEndian(encodedFrame, offset, kidLen);
		offset += kidLen;
	}

	if (!extendedCTRFlag) {
		ctr = ctrField;
	} else {
		size_t ctrLen = size_t(ctrField) + 1;
		ctr = sframe::DecodeBigEndian(encodedFrame, offset, ctrLen);
		offset += ctrLen;
	}

	SFrameHeaderInfo decodedHeader = {kid, ctr, offset};

	return decodedHeader;
}

} // namespace rtc::impl

#endif // RTC_ENABLE_MEDIA
