/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RELIABILITY_H
#define RTC_RELIABILITY_H

#include "common.hpp"

#include <chrono>

namespace rtc {

struct Reliability {
	// It true, the channel does not enforce message ordering and out-of-order delivery is allowed
	bool unordered = false;

	// If both maxPacketLifeTime or maxRetransmits are unset, the channel is reliable.
	// If either maxPacketLifeTime or maxRetransmits is set, the channel is unreliable.
	// (The settings are exclusive so both maxPacketLifetime and maxRetransmits must not be set.)

	// Time window during which transmissions and retransmissions may occur
	optional<std::chrono::milliseconds> maxPacketLifeTime;

	// Maximum number of retransmissions that are attempted
	optional<unsigned int> maxRetransmits;

	// For backward compatibility, do not use
	enum class Type { Reliable = 0, Rexmit, Timed };
	union {
		Type typeDeprecated = Type::Reliable;
		[[deprecated("Use maxPacketLifeTime or maxRetransmits")]] Type type;
	};
	variant<int, std::chrono::milliseconds> rexmit = 0;
};

} // namespace rtc

#endif
