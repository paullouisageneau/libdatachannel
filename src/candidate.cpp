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
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
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

Candidate::Candidate(string candidate, string mid)
    : mFamily(Family::Unresolved), mType(Type::Unknown), mTransportType(TransportType::Unknown),
      mPort(0), mPriority(0) {

	const std::array prefixes{"a=", "candidate:"};
	for (const string &prefix : prefixes)
		if (hasprefix(candidate, prefix))
			candidate.erase(0, prefix.size());

	mCandidate = std::move(candidate);
	mMid = std::move(mid);
}

bool Candidate::resolve(ResolveMode mode) {
	using TypeMap_t = std::unordered_map<string, Type>;	
	using TcpTypeMap_t = std::unordered_map<string, TransportType>;

	static const TypeMap_t TypeMap = {{"host", Type::Host},
	                                  {"srflx", Type::ServerReflexive},
	                                  {"prflx", Type::PeerReflexive},
	                                  {"relay", Type::Relayed}};

	static const TcpTypeMap_t TcpTypeMap = {{"active", TransportType::TcpActive},
	                                        {"passive", TransportType::TcpPassive},
	                                        {"so", TransportType::TcpSo}};

	if (mFamily != Family::Unresolved)
		return true;

	if(mCandidate.empty())
		throw std::logic_error("Candidate is empty");

	PLOG_VERBOSE << "Resolving candidate (mode="
				 << (mode == ResolveMode::Simple ? "simple" : "lookup")
				 << "): " << mCandidate;

	// See RFC 8445 for format
	std::istringstream iss(mCandidate);
	int component{0}, priority{0};
	string foundation, transport, node, service, typ_, type;
	if (iss >> foundation >> component >> transport >> priority &&
	    iss >> node >> service >> typ_ >> type && typ_ == "typ") {

		string left;
		std::getline(iss, left);

		if (auto it = TypeMap.find(type); it != TypeMap.end())
			mType = it->second;
		else
			mType = Type::Unknown;
		
		if (transport == "UDP" || transport == "udp") {
			mTransportType = TransportType::Udp;
		}
		else if (transport == "TCP" || transport == "tcp") {
			std::istringstream iss(left);
			string tcptype_, tcptype;
			if(iss >> tcptype_ >> tcptype && tcptype_ == "tcptype") {
				if (auto it = TcpTypeMap.find(tcptype); it != TcpTypeMap.end())
					mTransportType = it->second;
				else 
					mTransportType = TransportType::TcpUnknown;

			} else {
				mTransportType = TransportType::TcpUnknown;
			}
		} else {
			mTransportType = TransportType::Unknown;
		}

		// Try to resolve the node
		struct addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_flags = AI_ADDRCONFIG;
		if (mTransportType == TransportType::Udp) {
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_protocol = IPPROTO_UDP;
		}
		else if (mTransportType != TransportType::Unknown) {
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;
		}

		if (mode == ResolveMode::Simple)
			hints.ai_flags |= AI_NUMERICHOST;

		struct addrinfo *result = nullptr;
		if (getaddrinfo(node.c_str(), service.c_str(), &hints, &result) == 0) {
			for (auto p = result; p; p = p->ai_next) {
				if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
					// Rewrite the candidate
					char nodebuffer[MAX_NUMERICNODE_LEN];
					char servbuffer[MAX_NUMERICSERV_LEN];
					if (getnameinfo(p->ai_addr, socklen_t(p->ai_addrlen), nodebuffer,
					                MAX_NUMERICNODE_LEN, servbuffer, MAX_NUMERICSERV_LEN,
					                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
						
						mAddress = nodebuffer;
						mPort = uint16_t(std::stoul(servbuffer));
						mFamily = p->ai_family == AF_INET6 ? Family::Ipv6 : Family::Ipv4;
						
						const char sp{' '};
						std::ostringstream oss;
						oss << foundation << sp << component << sp << transport << sp << priority;
						oss << sp << nodebuffer << sp << servbuffer << sp << "typ" << sp << type;
						oss << left;
						mCandidate = oss.str();

						PLOG_VERBOSE << "Resolved candidate: " << mCandidate;
						break;
					}
				}
			}

			freeaddrinfo(result);
		}
	}

	return mFamily != Family::Unresolved;
}

string Candidate::candidate() const { return "candidate:" + mCandidate; }

string Candidate::mid() const { return mMid; }

Candidate::operator string() const {
	std::ostringstream line;
	line << "a=" << candidate();
	return line.str();
}

bool Candidate::isResolved() const { return mFamily != Family::Unresolved; }

Candidate::Family Candidate::family() const {
	return mFamily;

}
Candidate::Type Candidate::type() const {
	return mType;
}

std::optional<string> Candidate::address() const {
	return isResolved() ? std::make_optional(mAddress) : nullopt;
}

std::optional<uint16_t> Candidate::port() const {
	return isResolved() ? std::make_optional(mPort) : nullopt;
}

std::optional<uint32_t> Candidate::priority() const {
	return isResolved() ? std::make_optional(mPriority) : nullopt;
}

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Candidate &candidate) {
	return out << std::string(candidate);
}

std::ostream &operator<<(std::ostream &out, const rtc::Candidate::Type &type) {
	switch (type) {
	case rtc::Candidate::Type::Host:
		return out << "host";
	case rtc::Candidate::Type::PeerReflexive:
		return out << "peer_reflexive";
	case rtc::Candidate::Type::ServerReflexive:
		return out << "server_reflexive";
	case rtc::Candidate::Type::Relayed:
		return out << "relayed";
	default:
		return out << "unknown";
	}
}

std::ostream &operator<<(std::ostream &out, const rtc::Candidate::TransportType &transportType) {
	switch (transportType) {
	case rtc::Candidate::TransportType::Udp:
		return out << "UDP";
	case rtc::Candidate::TransportType::TcpActive:
		return out << "TCP_active";
	case rtc::Candidate::TransportType::TcpPassive:
		return out << "TCP_passive";
	case rtc::Candidate::TransportType::TcpSo:
		return out << "TCP_so";
	case rtc::Candidate::TransportType::TcpUnknown:
		return out << "TCP_unknown";
	default:
		return out << "unknown";
	}
}
