/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef sframecommon_hpp
#define sframecommon_hpp

#include "rtc/rtc.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <variant>

using namespace std::chrono_literals;

// SFrame does not distribute keys. A real deployment takes them from a group key agreement such as
// MLS, or from a key server, and this example stands in for that with a fresh random key per run
// that you copy between the two terminals alongside the SDP.
//
// Nothing here is a fixed key on purpose. An example with a hard-coded key gets copied into real
// products, and a key in source control is a key everyone has.
//
// RFC 9605 Section 4.4.1: "A given base_key MUST NOT be used for encryption by multiple senders"
// and "Implementations MUST mark each base_key as usable for encryption or decryption, never
// both." So each peer generates its OWN sending key and never encrypts with the one it received.
// Sharing one key across both directions would have each peer encrypt frame N under the same key
// and counter -- the same nonce -- which on the GCM suites recovers the authentication key and on
// the CTR suites recovers the plaintexts by XOR. Sharing one KID is as bad the other way: the KID
// is how a receiver picks a key, so two keys under one are indistinguishable to it.
// A cipher suite per media kind, as RFC 9605 Section 4.5 suggests: a short tag on audio, where
// frames are small and frequent, a longer one on video, where the 6 extra bytes cost nothing
// proportionally. Section 7.5 bounds what a truncated tag gives up -- carry SFrame only over a
// hop-by-hop-secure channel, and treat an elevated packet arrival rate as an attack signal.
const uint16_t kVideoCipherSuite = 0x06; // AES_256_CTR_HMAC_SHA512_80, 10-byte tag
const uint16_t kAudioCipherSuite = 0x08; // AES_256_CTR_HMAC_SHA512_32, 4-byte tag

// Both suites use AES-256 with HMAC-SHA-512. RFC 9605 sets no length requirement on the base key --
// it is HKDF input keying material -- but 16 bytes would be silently stretched to 128-bit security
// on a suite whose name says 256.
const size_t kBaseKeySize = 32;

const uint8_t kRatchetStepBits = 8; // low 8 bits of the KID carry the ratchet step
const uint16_t kRatchetPeriod = 0;  // seconds; 0 disables ratcheting

enum class Kind { Video, Audio };

const char *kindName(Kind kind) { return kind == Kind::Video ? "video" : "audio"; }

// What one direction of one m-line needs: the base key to derive from, and the full KID naming it.
// Each m-line gets its own, so no base key is ever shared across suites or directions.
struct KeyMaterial {
	rtc::binary baseKey;
	uint64_t kid = 0;
};

// One device for everything this process draws, rather than constructing one per call. Only
// used during single-threaded startup, before the send thread exists, so it needs no lock.
//
// std::random_device reads the OS CSPRNG on every platform this example targets, and is the
// strongest source reachable here: the library's own utils::random_bytes_engine() is an
// mt19937 meant for WebSocket masking, not key material, and is not public anyway. Where the
// library needs crypto-grade bytes itself -- certificate serials -- it calls the TLS backend
// directly. A real deployment takes key material from its key management system.
std::random_device &entropy() {
	static std::random_device rd;
	return rd;
}

// Four bytes per call rather than one, which is what result_type carries on every implementation
// this builds against.
rtc::binary randomBytes(size_t count) {
	static_assert(sizeof(std::random_device::result_type) == 4, "expected a 32-bit result_type");
	auto &rd = entropy();

	rtc::binary bytes;
	bytes.reserve(count);
	while (bytes.size() < count) {
		const auto word = rd();
		for (unsigned shift = 0; shift < 32 && bytes.size() < count; shift += 8)
			bytes.push_back(std::byte(uint8_t((word >> shift) & 0xFF)));
	}
	return bytes;
}

// A fresh sending key: kBaseKeySize bytes, and a random key generation with the ratchet step left
// at 0. The generation occupies the bits above kRatchetStepBits, so it is drawn small enough that
// composing it cannot overflow the KID.
KeyMaterial generateSendKey() {
	const uint64_t generation = (uint64_t(entropy()()) & 0xFFFFFF) + 1; // never 0, so visibly set
	return {randomBytes(kBaseKeySize), generation << kRatchetStepBits};
}

std::string toHex(const rtc::binary &data) {
	std::ostringstream out;
	out << std::hex << std::setfill('0');
	for (auto b : data)
		out << std::setw(2) << unsigned(std::to_integer<uint8_t>(b));
	return out.str();
}

// Returns false on anything that is not an even-length run of hex digits, so a mistyped paste is
// rejected rather than silently turned into a different key.
bool fromHex(const std::string &text, rtc::binary &out) {
	if (text.empty() || text.size() % 2 != 0)
		return false;

	rtc::binary parsed;
	parsed.reserve(text.size() / 2);
	for (size_t i = 0; i < text.size(); i += 2) {
		if (!std::isxdigit(static_cast<unsigned char>(text[i])) ||
		    !std::isxdigit(static_cast<unsigned char>(text[i + 1])))
			return false;
		parsed.push_back(std::byte(std::stoul(text.substr(i, 2), nullptr, 16)));
	}
	out = std::move(parsed);
	return true;
}

// One line the other peer pastes back in, naming which m-line it belongs to. The KID is decimal
// and the key is hex.
void printSendKey(Kind kind, const KeyMaterial &key) {
	std::cout << "Our " << kindName(kind)
	          << " sending key (paste this to the other peer with command 3):" << std::endl
	          << kindName(kind) << " " << key.kid << " " << toHex(key.baseKey) << std::endl
	          << std::endl;
}

// Installs the peer's sending key for decryption, refusing it if it collides with our own.
//
// The realistic way that happens is the obvious operator error in a copy-and-paste flow:
// pasting a terminal's own key line back into itself. The check is a cheap assertion of the
// invariant either way -- sharing a base key means both ends encrypt frame N under the same key
// and counter, one nonce for two plaintexts (RFC 9605 Section 4.4.1), and sharing a KID leaves
// the receiver unable to tell the two keys apart.
//
// @return false if the peer's key was refused, with the reason printed.
bool installPeerKey(const std::shared_ptr<rtc::SFrameReceiveKeyProvider> &provider,
                    const KeyMaterial &ours, const KeyMaterial &peer) {
	const uint64_t ourGeneration = ours.kid >> kRatchetStepBits;
	const uint64_t peerGeneration = peer.kid >> kRatchetStepBits;

	if (peerGeneration == ourGeneration) {
		std::cout << "** Refused: the peer's key generation (" << peerGeneration
		          << ") is the one we send under. Each direction needs its own, or the receiver "
		             "cannot tell the two keys apart. **"
		          << std::endl;
		return false;
	}

	if (peer.baseKey == ours.baseKey) {
		std::cout << "** Refused: the peer's base key is identical to ours. RFC 9605 Section 4.4.1 "
		             "forbids one base key encrypting for two senders -- both would reuse the same "
		             "nonces. Did this terminal's own key get pasted back into it? **"
		          << std::endl;
		return false;
	}

	provider->addKey(peer.kid, rtc::SFrameReceiveKey{peer.baseKey});
	std::cout << "[Remote sending key installed for KID " << peer.kid << "]" << std::endl;
	return true;
}

// What one m-line of an answer actually agreed to.
//
// This has to be read from the answer itself. libdatachannel propagates only RTX and a=sframe
// changes onto the offerer's own track description, so that description still lists every codec the
// offer carried and cannot say which one was chosen.
struct NegotiatedMedia {
	bool found = false;
	uint8_t payloadType = 0; // the codec the answerer chose, 0 if it named none
	bool sframe = false;     // a=sframe survived, so the media is SFrame protected
};

NegotiatedMedia negotiatedMedia(const rtc::Description &answer, const std::string &mid) {
	NegotiatedMedia result;
	for (int i = 0; i < answer.mediaCount(); ++i) {
		// media(i) hands back a variant by value, so it is held in a local before being inspected.
		auto entry = answer.media(i);
		auto media = std::get_if<const rtc::Description::Media *>(&entry);
		if (!media || !*media || (*media)->mid() != mid)
			continue;

		result.found = true;
		result.sframe = (*media)->hasSFrame();

		// The first non-RTX payload type is the codec the answerer chose: SDP lists formats in the
		// sender's order of preference, and for an answer that sender is the answerer.
		for (int payloadType : (*media)->payloadTypes()) {
			// A static payload type carries no a=rtpmap (RFC 4566) and rtpMap() throws for it.
			if (!(*media)->hasPayloadType(payloadType))
				continue;
			const auto *map = (*media)->rtpMap(payloadType);
			if (map->format == "rtx" || map->format == "RTX")
				continue;
			result.payloadType = uint8_t(payloadType);
			break;
		}
		break;
	}
	return result;
}

// @param accept Consulted for each remote description before it is applied. Returning false
//               discards it. Use it to inspect what the peer agreed to -- see sender.cpp.
// @param onPeerKey Called with the peer's sending key for one m-line, to install as a decryption
//                  key. Without it command 3 is refused, since nothing could be done with the
//                  value.
// @param onNegotiated Called with each remote description once it has been applied. An offerer
//                     builds its media chains here: this is the first moment at which both the
//                     negotiated codec and the fate of a=sframe are known.
void runSignalling(const std::shared_ptr<rtc::PeerConnection> &pc,
                   const std::function<bool(const rtc::Description &)> &accept = nullptr,
                   const std::function<void(Kind, const KeyMaterial &)> &onPeerKey = nullptr,
                   const std::function<void(const rtc::Description &)> &onNegotiated = nullptr) {
	bool exit = false;
	while (!exit) {
		std::cout << std::endl
		          << "* 0: Exit / 1: Enter remote description / 2: Enter remote candidate / "
		             "3: Enter remote sending key *"
		          << std::endl
		          << "[Command]: ";

		int command = -1;
		std::cin >> command;
		std::cin.ignore();

		switch (command) {
		case 0:
			exit = true;
			break;
		case 1: {
			std::cout << "[Description]: ";
			std::string sdp, line;
			while (getline(std::cin, line) && !line.empty()) {
				sdp += line;
				sdp += "\r\n";
			}
			rtc::Description remote(sdp);
			if (accept && !accept(remote)) {
				std::cout << "[Remote description rejected]" << std::endl;
				break;
			}
			pc->setRemoteDescription(std::move(remote));
			// Passed back as the library now holds it, rather than as it was handed in.
			if (onNegotiated) {
				if (auto applied = pc->remoteDescription())
					onNegotiated(*applied);
			}
			break;
		}
		case 2: {
			std::cout << "[Candidate]: ";
			std::string candidate;
			getline(std::cin, candidate);
			pc->addRemoteCandidate(candidate);
			break;
		}
		case 3: {
			if (!onPeerKey) {
				std::cout << "** This peer does not receive media **" << std::endl;
				break;
			}
			std::cout << "[Remote sending key, \"<video|audio> <kid> <hex>\"]: ";
			std::string line;
			getline(std::cin, line);
			std::istringstream in(line);
			std::string which, hex;
			uint64_t kid = 0;
			rtc::binary baseKey;
			if (!(in >> which >> kid >> hex) || (which != "video" && which != "audio") ||
			    !fromHex(hex, baseKey)) {
				std::cout << "** Expected \"video\" or \"audio\", a decimal KID, and an "
				             "even-length hex key **"
				          << std::endl;
				break;
			}
			onPeerKey(which == "video" ? Kind::Video : Kind::Audio,
			          KeyMaterial{std::move(baseKey), kid});
			break;
		}
		default:
			std::cout << "** Invalid Command **" << std::endl;
			break;
		}
	}
}

void printLocalDescription(rtc::Description description) {
	std::cout << "Local Description (Paste this to the other peer):" << std::endl;
	std::cout << std::string(description) << std::endl;
}

void printLocalCandidate(rtc::Candidate candidate) {
	std::cout << "Local Candidate (Paste this to the other peer after the description):"
	          << std::endl;
	std::cout << std::string(candidate) << std::endl << std::endl;
}

#endif /* sframecommon_hpp */
