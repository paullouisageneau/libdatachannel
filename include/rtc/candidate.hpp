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

#include "common.hpp"

#include <string>

namespace rtc {

class RTC_CPP_EXPORT Candidate {
public:
	enum class Family { Unresolved, Ipv4, Ipv6 };
	enum class Type { Unknown, Host, ServerReflexive, PeerReflexive, Relayed };
	enum class TransportType { Unknown, Udp, TcpActive, TcpPassive, TcpSo, TcpUnknown };

	Candidate();
	Candidate(string candidate);
	Candidate(string candidate, string mid);

	void hintMid(string mid);

	enum class ResolveMode { Simple, Lookup };
	bool resolve(ResolveMode mode = ResolveMode::Simple);

	Type type() const;
	TransportType transportType() const;
	uint32_t priority() const;
	string candidate() const;
	string mid() const;
	operator string() const;

	bool operator==(const Candidate &other) const;
	bool operator!=(const Candidate &other) const;

	bool isResolved() const;
	Family family() const;
	optional<string> address() const;
	optional<uint16_t> port() const;

private:
	void parse(string candidate);

	string mFoundation;
	uint32_t mComponent, mPriority;
	string mTypeString, mTransportString;
	Type mType;
	TransportType mTransportType;
	string mNode, mService;
	string mTail;

	optional<string> mMid;

	// Extracted on resolution
	Family mFamily;
	string mAddress;
	uint16_t mPort;
};

} // namespace rtc

RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, const rtc::Candidate &candidate);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, const rtc::Candidate::Type &type);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out,
                                        const rtc::Candidate::TransportType &transportType);

#endif
