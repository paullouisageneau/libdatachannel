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

#if not USE_JUICE
enum class CandidateType { Host = 0, ServerReflexive, PeerReflexive, Relayed };
enum class CandidateTransportType { Udp = 0, TcpActive, TcpPassive, TcpSo };
struct CandidateInfo {
	string address;
	int port;
	CandidateType type;
	CandidateTransportType transportType;
};
#endif

class Candidate {
public:
	Candidate(string candidate, string mid = "");

	enum class ResolveMode { Simple, Lookup };
	bool resolve(ResolveMode mode = ResolveMode::Simple);
	bool isResolved() const;

	string candidate() const;
	string mid() const;
	operator string() const;

private:
	string mCandidate;
	string mMid;
	bool mIsResolved;
};

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Candidate &candidate);
#if not USE_JUICE
std::ostream &operator<<(std::ostream &out, const rtc::CandidateType &type);
std::ostream &operator<<(std::ostream &out, const rtc::CandidateTransportType &transportType);
#endif

#endif

