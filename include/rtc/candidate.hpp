/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#ifndef RTC_CANDIDATE_H
#define RTC_CANDIDATE_H

#include "include.hpp"

#include <string>

namespace rtc {

class Candidate {
public:
	enum class Family { Unresolved, Ipv4, Ipv6 };
	enum class Type { Unknown, Host, ServerReflexive, PeerReflexive, Relayed };
	enum class TransportType { Unknown, Udp, TcpActive, TcpPassive, TcpSo, TcpUnknown };

	Candidate(string candidate = "", string mid = "");

	enum class ResolveMode { Simple, Lookup };
	bool resolve(ResolveMode mode = ResolveMode::Simple);

	string candidate() const;
	string mid() const;
	operator string() const;

	bool isResolved() const;
	Family family() const;
	Type type() const;
	std::optional<string> address() const;
	std::optional<uint16_t> port() const;
	std::optional<uint32_t> priority() const;

private:
	string mCandidate;
	string mMid;

	// Extracted on resolution
	Family mFamily;
	Type mType;
	TransportType mTransportType;
	string mAddress;
	uint16_t mPort;
	uint32_t mPriority;
};

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Candidate &candidate);
std::ostream &operator<<(std::ostream &out, const rtc::Candidate::Type &type);
std::ostream &operator<<(std::ostream &out, const rtc::Candidate::TransportType &transportType);

#endif

