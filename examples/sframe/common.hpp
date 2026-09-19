/**
 * Copyright (c) 2025-2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// SFrame does not distribute keys. Both peers are assumed to have agreed the material below
// out of band -- a group key agreement such as MLS, or a key server -- before any media
// flows. It is hard-coded here only so the two processes need no extra signalling channel.
//
// RFC 9605 Section 4.4.1: "A given base_key MUST NOT be used for encryption by multiple
// senders" and "Implementations MUST mark each base_key as usable for encryption or
// decryption, never both."
//
// So each direction gets its own base key AND its own key generation, and neither side may
// ever encrypt with the other's. Reusing one key in both directions would have both peers
// encrypt frame N under the same key with the same counter, i.e. the same nonce: on the GCM
// suites that recovers the authentication key and lets either direction be forged, and on the
// CTR suites XORing the two ciphertexts recovers the plaintexts. Reusing one key generation
// across the two keys is just as bad in the other direction -- the KID is what tells the
// receiver which key to use, so two keys sharing a KID cannot be told apart.
const uint16_t kCipherSuite = 0x04; // AES_128_GCM_SHA256_128

// Sender -> receiver. The sender encrypts with this; the receiver only ever decrypts with it.
const uint64_t kSenderKeyGeneration = 1;

// Receiver -> sender, if the receiver ever sends. Present to make the split explicit: a
// second direction needs its own key and its own generation, not a copy of the first.
const uint64_t kReceiverKeyGeneration = 2;

const uint8_t kRatchetStepBits = 8; // low 8 bits of the KID carry the ratchet step
const uint64_t kRatchetPeriod = 0;  // seconds; 0 disables ratcheting

// Distinct material per direction. A real deployment takes both from its key agreement; they
// are unrelated keys, not one key with a tweak.
rtc::binary senderBaseKey() {
	rtc::binary key(16);
	for (size_t i = 0; i < key.size(); ++i)
		key[i] = std::byte(0x40 + i);
	return key;
}

rtc::binary receiverBaseKey() {
	rtc::binary key(16);
	for (size_t i = 0; i < key.size(); ++i)
		key[i] = std::byte(0x90 + i);
	return key;
}

// Answers with the base key for a key generation, and nullopt for anything else -- which is
// what makes the decoder refuse forged frames. The library splits the KID before calling, so
// there is no KID arithmetic here.
//
// This provider is decrypt-only: it is handed to an SFrameDecoder and the keys it returns are
// never used to encrypt. Keep that separation, per RFC 9605 Section 4.4.1.
class PeerKeyProvider final : public rtc::SFrameKeyProvider {
public:
	// The generation this peer expects to receive under, and the key for it.
	PeerKeyProvider(uint64_t keyGeneration, rtc::binary baseKey)
	    : mKeyGeneration(keyGeneration), mBaseKey(std::move(baseKey)) {}

	// This example lets the library derive a key per SSRC, so each track is independent and
	// gets its own counter. Not negotiated in SDP, so it must match the sender's
	// SFrameConfig::perSsrcDerivation. Answer false on both sides instead to interoperate
	// with a plain RFC 9605 peer -- then every track shares one key and the senders must
	// share one encoder.
	bool usePerSSRCDerivation() const override { return true; }

	uint8_t ratchetStepBits() const override { return kRatchetStepBits; }

	std::optional<rtc::SFrameKeyDetails> getKeyDetails(uint64_t keyGeneration) override {
		if (keyGeneration != mKeyGeneration)
			return std::nullopt;

		rtc::SFrameKeyDetails details;
		details.cipherSuiteId = kCipherSuite;
		details.baseKey = mBaseKey;
		// Decrypt-only, per RFC 9605 Section 4.4.1. SFrameEncoder refuses a key marked this
		// way, so the sending key cannot be handed back here by mistake.
		details.use = rtc::SFrameKeyUse::Decrypt;
		return details;
	}

private:
	const uint64_t mKeyGeneration;
	const rtc::binary mBaseKey;
};

void runSignalling(const std::shared_ptr<rtc::PeerConnection> &pc) {
	bool exit = false;
	while (!exit) {
		std::cout << std::endl
		          << "* 0: Exit / 1: Enter remote description / 2: Enter remote candidate *"
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
			pc->setRemoteDescription(sdp);
			break;
		}
		case 2: {
			std::cout << "[Candidate]: ";
			std::string candidate;
			getline(std::cin, candidate);
			pc->addRemoteCandidate(candidate);
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
