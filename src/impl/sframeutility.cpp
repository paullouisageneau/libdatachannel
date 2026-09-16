/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "impl/sframeutility.hpp"

#include "impl/logcounter.hpp"
#include "impl/sframecrypto.hpp"
#include "rtp.hpp"

#include <algorithm>
#include <stdexcept>

namespace rtc::impl {

namespace {

LogCounter COUNTER_SFRAME_DECODE_FAILED(plog::warning,
                                        "Number of SFrame frames that failed to decode");

} // namespace

binary SFrameUtility::MakeAAD(const binary& header, const binary& metadata) {
	binary aad;
	aad.reserve(header.size() + metadata.size());
	aad.insert(aad.end(), header.begin(), header.end());
	aad.insert(aad.end(), metadata.begin(), metadata.end());
	return aad;
}

SFrameKeyMaterial SFrameUtility::GetSFrameKeys(const uint64_t kid,
                                               const uint64_t ctr,
                                               const binary baseKey,
                                               const uint16_t cipherSuiteId) {
	binary keyLabelReal = SFrameUtility::GetSFrameKeyLabel(kid, cipherSuiteId);
	binary saltLabel = SFrameUtility::GetSFrameSaltLabel(kid, cipherSuiteId);

	impl::SFrameKeyInfo keyInfo = { baseKey, cipherSuiteId, keyLabelReal, saltLabel };
	impl::SFrameDerivedKeys derivedKeys = impl::SFrameCrypto::DeriveSFrameKeys(keyInfo);

	size_t nonceSize = impl::SFrameCrypto::GetNonceSize(cipherSuiteId);
	binary nonce = SFrameUtility::DeriveNonce(derivedKeys.sframeSalt, ctr, nonceSize);
	SFrameKeyMaterial keyMaterial = {derivedKeys.contentEncryptionKey, nonce};
	return keyMaterial;
}

binary SFrameUtility::GetSFrameKeyLabel(const uint64_t kid, const uint16_t cipherSuiteId) {
	binary label;
	static constexpr char PREFIX[] = "SFrame 1.0 Secret key ";

	label.insert(label.end(), reinterpret_cast<const std::byte *>(PREFIX),
	             reinterpret_cast<const std::byte *>(PREFIX) + sizeof(PREFIX) - 1);

	auto kidBytes = SFrameUtility::EncodeBigEndian(kid, 8);
	label.insert(label.end(), kidBytes.begin(), kidBytes.end());

	auto cipherSuiteBytes = SFrameUtility::EncodeBigEndian(cipherSuiteId, 2);
	label.insert(label.end(), cipherSuiteBytes.begin(), cipherSuiteBytes.end());
	return label;
}

binary SFrameUtility::GetSFrameSaltLabel(const uint64_t kid, const uint16_t cipherSuiteId) {
	binary label;
	static constexpr char PREFIX[] = "SFrame 1.0 Secret salt ";

	label.insert(label.end(), reinterpret_cast<const std::byte *>(PREFIX),
	             reinterpret_cast<const std::byte *>(PREFIX) + sizeof(PREFIX) - 1);

	auto kidBytes = SFrameUtility::EncodeBigEndian(kid, 8);
	label.insert(label.end(), kidBytes.begin(), kidBytes.end());

	auto cipherSuiteBytes = SFrameUtility::EncodeBigEndian(cipherSuiteId, 2);
	label.insert(label.end(), cipherSuiteBytes.begin(), cipherSuiteBytes.end());
	return label;
}

binary SFrameUtility::DeriveNonce(const binary& sframeSalt, const uint64_t ctr, size_t nonceSize) {
	//Encode ctr as big-endian, right-aligned in nonce_size bytes
	 binary ctrBytes(nonceSize, std::byte{0});
	 for (size_t i = 0; i < sizeof(ctr) && i < nonceSize; ++i) {
		 ctrBytes[nonceSize - 1 - i] =
			 static_cast<std::byte>((ctr >> (8 * i)) & 0xFF);
	 }

	// The salt is derived at the cipher suite's nonce size, but this is exported: a
	// caller passing a shorter salt would read past its end.
	if (sframeSalt.size() < nonceSize)
		throw std::invalid_argument("SFrame salt is shorter than the nonce size");

	// XOR sframeSalt with ctrBytes
	binary nonce(nonceSize);
	for (size_t i = 0; i < nonceSize; ++i) {
		nonce[i] = sframeSalt[i] ^ ctrBytes[i];
	}

	return nonce;
}

binary SFrameUtility::EncodePlaintext(const uint16_t cipherSuiteId,
                                                    const binary& sframeKey,
                                                    const binary& nonce,
                                                    const binary& aad,
                                                    const binary& plaintext) {
	return  impl::SFrameCrypto::EncodePlaintext(cipherSuiteId, sframeKey, nonce, aad, plaintext);
}

binary SFrameUtility::DecodeCiphertext(const uint16_t cipherSuiteId,
                                                     const binary& sframeKey,
                                                     const binary& nonce,
                                                     const binary& aad,
                                                     const binary& cipherText) {
	return  impl::SFrameCrypto::DecodeCiphertext(cipherSuiteId, sframeKey, nonce, aad, cipherText);
}

size_t SFrameUtility::MinimalByteLength(uint64_t v) {
	for (int i = 7; i >= 0; --i) {
		if ((v >> (8 * i)) != 0) {
			return i + 1;
		}
	}
	return 1;
}

binary SFrameUtility::EncodeBigEndian(uint64_t value, size_t numBytes) {
	binary out(numBytes);
	for (size_t i = 0; i < numBytes; ++i) {
		out[numBytes - 1 - i] = std::byte((value >> (8 * i)) & 0xFF);
	}
	return out;
}

uint64_t SFrameUtility::DecodeBigEndian(const binary& buf, size_t offset, size_t len) {
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

uint64_t SFrameUtility::MakeKid(uint64_t keyGeneration, uint64_t ratchetStep,
                                uint8_t ratchetStepBits) {
	if (ratchetStepBits > MaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(MaxRatchetStepBits));

	// The generation is shifted up by R, so anything that does not fit in the remaining bits
	// would wrap and alias another generation -- and the KID is an HKDF label, so two
	// generations sharing a KID share a key.
	if (ratchetStepBits > 0 && keyGeneration >= (uint64_t(1) << (64 - ratchetStepBits)))
		throw std::invalid_argument("SFrame key generation does not fit in 64 bits alongside " +
		                            std::to_string(ratchetStepBits) + " ratchet step bits");

	const uint64_t mask = ratchetMask(ratchetStepBits);
	return (keyGeneration << ratchetStepBits) | (ratchetStep & mask);
}

uint64_t SFrameUtility::KeyGenerationFromKid(uint64_t kid, uint8_t ratchetStepBits) {
	if (ratchetStepBits > MaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(MaxRatchetStepBits));

	return kid >> ratchetStepBits;
}

uint64_t SFrameUtility::RatchetStepFromKid(uint64_t kid, uint8_t ratchetStepBits) {
	if (ratchetStepBits > MaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(MaxRatchetStepBits));

	return kid & ratchetMask(ratchetStepBits);
}

uint64_t SFrameUtility::NextRatchetKid(uint64_t kid, uint8_t ratchetStepBits) {
	if (ratchetStepBits > MaxRatchetStepBits)
		throw std::invalid_argument("SFrame ratchetStepBits must be at most " +
		                            std::to_string(MaxRatchetStepBits));

	// ratchet_step % (1 << R): the step wraps to zero and the key generation above it is
	// left alone, so the ratchet never leaves the KID range the application allocated.
	const uint64_t mask = ratchetMask(ratchetStepBits);
	return (kid & ~mask) | ((kid + 1) & mask);
}

size_t SFrameUtility::MinBaseKeySize() {
	return impl::SFrameCrypto::MinBaseKeySize;
}

void SFrameUtility::ValidateBaseKey(const binary& baseKey) {
	impl::SFrameCrypto::ValidateBaseKey(baseKey);
}

binary SFrameUtility::DeriveSSRCKey(uint32_t ssrc, const binary& baseKey, uint16_t cipherSuiteId) {
	return impl::SFrameCrypto::DeriveSSRCKey(ssrc, baseKey, cipherSuiteId);
}

bool SFrameUtility::IsValidDescriptor(uint8_t descriptor, bool isFirst, bool isLast) {
	// Exact equality, so this also requires T=0 (raw origin) and the 5 reserved bits to be
	// zero. A packetized-origin object (T=1) is rejected rather than mis-delivered.
	const uint8_t expected = uint8_t((isFirst ? SFRAME_DESCRIPTOR_S : 0) |
	                                 (isLast ? SFRAME_DESCRIPTOR_E : 0));
	return descriptor == expected;
}

bool SFrameUtility::IsPacketizedOrigin(uint8_t descriptor) {
	return (descriptor & SFRAME_DESCRIPTOR_T) != 0;
}

bool SFrameUtility::ParseRtpPayload(const binary& packet, size_t& hdrSize, size_t& payloadEnd) {
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

bool SFrameUtility::ParseSFramePacket(const std::shared_ptr<Message>& msg, size_t& hdrSize,
                                      size_t& payloadEnd) {
	if (!ParseRtpPayload(*msg, hdrSize, payloadEnd))
		return false;

	// Must carry the 1-byte S/E/T descriptor plus at least one payload byte
	// (after removing any padding).
	return payloadEnd >= hdrSize + 2;
}

binary SFrameUtility::RatchetKey(uint16_t cipherSuiteId, const binary& key) {
	return impl::SFrameCrypto::RatchetKey(cipherSuiteId, key);
}

void SFrameUtility::DecryptMessages(message_vector &messages,
                                    const std::shared_ptr<SFrameKeyProvider> &keyProvider,
                                    uint32_t ssrc, std::unique_ptr<SFrameDecoder> &decoder) {
	// Only reachable from a direct caller, since the in-tree depacketizers take a provider
	// at construction. Clear the batch first: ciphertext must never reach the application.
	if (!keyProvider) {
		messages.clear();
		throw std::invalid_argument("SFrame key provider is null");
	}

	// The decoder derives its keys from the SSRC, so a stream that changes SSRC has to be
	// rebound. Rebinding rather than replacing keeps the catch-up rate limit: a peer is free
	// to put a different SSRC on every packet, and a fresh decoder would hand each one a
	// fresh allowance to walk thousands of ratchet steps before anything authenticates.
	if (decoder && decoder->ssrc() != std::optional<uint32_t>(ssrc))
		decoder->rebindSsrc(ssrc);

	if (!decoder)
		decoder = std::make_unique<SFrameDecoder>(keyProvider, ssrc);

	message_vector decrypted;
	decrypted.reserve(messages.size());
	for (auto &msg : messages) {
		if (msg->type == Message::Control) {
			decrypted.push_back(std::move(msg));
			continue;
		}
		binary metadata;
		try {
			auto decrypted_msg = decoder->decodeFrame(msg, metadata);
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

// RFC 9605 Section 4.2 header:
//
//   0 1 2 3 4 5 6 7
//  +-+-+-+-+-+-+-+-+---------------------------+---------------------------+
//  |X|  K  |Y|  C  |   KID... (X: 1..8 bytes)  |   CTR... (Y: 1..8 bytes)  |
//  +-+-+-+-+-+-+-+-+---------------------------+---------------------------+
//
// X and Y are the extended flags. Clear, K and C hold the KID and CTR values themselves
// (0..7) and no further bytes follow; set, they hold length-1 of the big-endian field that
// follows.
binary SFrameHeader::Encode(SFrameHeaderInfo headerInfo) {
	bool useBaseKidHeader = (headerInfo.kid < 8);
	bool useBaseCTRHeader = (headerInfo.ctr < 8);

	size_t kidLen = useBaseKidHeader ? 1 : impl::SFrameUtility::MinimalByteLength(headerInfo.kid);
	size_t ctrLen = useBaseCTRHeader ? 1 : impl::SFrameUtility::MinimalByteLength(headerInfo.ctr);

	uint8_t config = 0;
	if (!useBaseKidHeader) config |= 0x80;  // X
	if (!useBaseCTRHeader) config |= 0x08;  // Y

	uint8_t kidField = uint8_t(useBaseKidHeader ? headerInfo.kid : (kidLen - 1)) & 0x07;
	config |= uint8_t(kidField << 4);

	uint8_t ctrField = uint8_t(useBaseCTRHeader ? headerInfo.ctr : (ctrLen - 1)) & 0x07;
	config |= ctrField;

	binary header;
	header.push_back(std::byte(config));

	if (!useBaseKidHeader) {
		auto kidBytes = impl::SFrameUtility::EncodeBigEndian(headerInfo.kid, kidLen);
		header.insert(header.end(), kidBytes.begin(), kidBytes.end());
	}

	if (!useBaseCTRHeader) {
		auto ctrBytes = impl::SFrameUtility::EncodeBigEndian(headerInfo.ctr, ctrLen);
		header.insert(header.end(), ctrBytes.begin(), ctrBytes.end());
	}

	return header;
}

// Inverse of Encode(); see the layout above.
SFrameHeaderInfo SFrameHeader::Decode(const binary& encodedFrame) {
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
		kid = impl::SFrameUtility::DecodeBigEndian(encodedFrame, offset, kidLen);
		offset += kidLen;
	}

	if (!extendedCTRFlag) {
		ctr = ctrField;
	} else {
		size_t ctrLen = size_t(ctrField) + 1;
		ctr = impl::SFrameUtility::DecodeBigEndian(encodedFrame, offset, ctrLen);
		offset += ctrLen;
	}

	SFrameHeaderInfo decodedHeader = {kid, ctr, offset};

	return decodedHeader;
}

} // namespace rtc::impl
