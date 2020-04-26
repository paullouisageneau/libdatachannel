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

#include "candidate.hpp"

#include <algorithm>
#include <array>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#include <sys/types.h>

using std::array;
using std::string;

namespace {

inline bool hasprefix(const string &str, const string &prefix) {
	return str.size() >= prefix.size() &&
	       std::mismatch(prefix.begin(), prefix.end(), str.begin()).first == prefix.end();
}

} // namespace

namespace rtc {

Candidate::Candidate(string candidate, string mid) : mIsResolved(false) {
	const std::array prefixes{"a=", "candidate:"};
	for (string prefix : prefixes)
		if (hasprefix(candidate, prefix))
			candidate.erase(0, prefix.size());

	mCandidate = std::move(candidate);
	mMid = std::move(mid);
}

bool Candidate::resolve(ResolveMode mode) {
	if (mIsResolved)
		return true;

	// See RFC 8445 for format
	std::stringstream ss(mCandidate);
	int component{0}, priority{0};
	string foundation, transport, node, service, typ_, type;
	if (ss >> foundation >> component >> transport >> priority &&
	    ss >> node >> service >> typ_ >> type && typ_ == "typ") {

		// Try to resolve the node
		struct addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_flags = AI_ADDRCONFIG;
		if (transport == "UDP" || transport == "udp") {
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_protocol = IPPROTO_UDP;
		}

		if (transport == "TCP" || transport == "tcp") {
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;
		}

		if (mode == ResolveMode::Simple)
			hints.ai_flags |= AI_NUMERICHOST;

		struct addrinfo *result = nullptr;
		if (getaddrinfo(node.c_str(), service.c_str(), &hints, &result) == 0) {
			for (auto p = result; p; p = p->ai_next)
				if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
					// Rewrite the candidate
					char nodebuffer[MAX_NUMERICNODE_LEN];
					char servbuffer[MAX_NUMERICSERV_LEN];
					if (getnameinfo(p->ai_addr, p->ai_addrlen, nodebuffer, MAX_NUMERICNODE_LEN,
					                servbuffer, MAX_NUMERICSERV_LEN,
					                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
						string left;
						std::getline(ss, left);
						const char sp{' '};
						ss.clear();
						ss << foundation << sp << component << sp << transport << sp << priority;
						ss << sp << nodebuffer << sp << servbuffer << sp << "typ" << sp << type;
						if (!left.empty())
							ss << left;
						mCandidate = ss.str();
						return mIsResolved = true;
					}
				}
		}

		freeaddrinfo(result);
	}

	return false;
}

bool Candidate::isResolved() const { return mIsResolved; }

string Candidate::candidate() const { return "candidate:" + mCandidate; }

string Candidate::mid() const { return mMid; }

Candidate::operator string() const {
	std::ostringstream line;
	line << "a=" << candidate();
	return line.str();
}

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Candidate &candidate) {
	return out << std::string(candidate);
}

#if not USE_JUICE
std::ostream &operator<<(std::ostream &out, const rtc::CandidateType &type) {
	switch (type) {
	case rtc::CandidateType::Host:
		return out << "Host";
	case rtc::CandidateType::PeerReflexive:
		return out << "PeerReflexive";
	case rtc::CandidateType::Relayed:
		return out << "Relayed";
	case rtc::CandidateType::ServerReflexive:
		return out << "ServerReflexive";
	default:
		return out << "Unknown";
	}
}

std::ostream &operator<<(std::ostream &out, const rtc::CandidateTransportType &transportType) {
	switch (transportType) {
	case rtc::CandidateTransportType::TcpActive:
		return out << "TcpActive";
	case rtc::CandidateTransportType::TcpPassive:
		return out << "TcpPassive";
	case rtc::CandidateTransportType::TcpSo:
		return out << "TcpSo";
	case rtc::CandidateTransportType::Udp:
		return out << "Udp";
	default:
		return out << "Unknown";
	}
}
#endif
