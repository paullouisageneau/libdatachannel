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
#include <cctype>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <sys/types.h>

using std::array;
using std::string;

namespace {

inline bool match_prefix(const string &str, const string &prefix) {
	return str.size() >= prefix.size() &&
	       std::mismatch(prefix.begin(), prefix.end(), str.begin()).first == prefix.end();
}

inline void trim_begin(string &str) {
	str.erase(str.begin(),
	          std::find_if(str.begin(), str.end(), [](char c) { return !std::isspace(c); }));
}

inline void trim_end(string &str) {
	str.erase(
	    std::find_if(str.rbegin(), str.rend(), [](char c) { return !std::isspace(c); }).base(),
	    str.end());
}

} // namespace

namespace rtc {

Candidate::Candidate()
    : mFoundation("none"), mComponent(0), mPriority(0), mTypeString("unknown"),
      mTransportString("unknown"), mType(Type::Unknown), mTransportType(TransportType::Unknown),
      mNode("0.0.0.0"), mService("9"), mFamily(Family::Unresolved), mPort(0) {}

Candidate::Candidate(string candidate) : Candidate() {
	if (!candidate.empty())
		parse(std::move(candidate));
}

Candidate::Candidate(string candidate, string mid) : Candidate() {
	if (!candidate.empty())
		parse(std::move(candidate));
	if (!mid.empty())
		mMid.emplace(std::move(mid));
}

void Candidate::parse(string candidate) {
	using TypeMap_t = std::unordered_map<string, Type>;
	using TcpTypeMap_t = std::unordered_map<string, TransportType>;

	static const TypeMap_t TypeMap = {{"host", Type::Host},
	                                  {"srflx", Type::ServerReflexive},
	                                  {"prflx", Type::PeerReflexive},
	                                  {"relay", Type::Relayed}};

	static const TcpTypeMap_t TcpTypeMap = {{"active", TransportType::TcpActive},
	                                        {"passive", TransportType::TcpPassive},
	                                        {"so", TransportType::TcpSo}};

	const std::array prefixes{"a=", "candidate:"};
	for (const string &prefix : prefixes)
		if (match_prefix(candidate, prefix))
			candidate.erase(0, prefix.size());

	PLOG_VERBOSE << "Parsing candidate: " << candidate;

	// See RFC 8445 for format
	std::istringstream iss(candidate);
	string transport, typ_, type;
	if (!(iss >> mFoundation >> mComponent >> mTransportString >> mPriority &&
	      iss >> mNode >> mService >> typ_ >> mTypeString && typ_ == "typ"))
		throw std::invalid_argument("Invalid candidate format");

	std::getline(iss, mTail);
	trim_begin(mTail);
	trim_end(mTail);

	if (auto it = TypeMap.find(mTypeString); it != TypeMap.end())
		mType = it->second;
	else
		mType = Type::Unknown;

	if (transport == "UDP" || transport == "udp") {
		mTransportType = TransportType::Udp;
	} else if (transport == "TCP" || transport == "tcp") {
		// Peek tail to find TCP type
		std::istringstream iss(mTail);
		string tcptype_, tcptype;
		if (iss >> tcptype_ >> tcptype && tcptype_ == "tcptype") {
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
}

void Candidate::hintMid(string mid) {
	if (!mMid)
		mMid.emplace(std::move(mid));
}

bool Candidate::resolve(ResolveMode mode) {
	PLOG_VERBOSE << "Resolving candidate (mode="
	             << (mode == ResolveMode::Simple ? "simple" : "lookup") << "): " << mNode << ' '
	             << mService;

	// Try to resolve the node and service
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = AI_ADDRCONFIG;
	if (mTransportType == TransportType::Udp) {
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
	} else if (mTransportType != TransportType::Unknown) {
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
	}

	if (mode == ResolveMode::Simple)
		hints.ai_flags |= AI_NUMERICHOST;

	struct addrinfo *result = nullptr;
	if (getaddrinfo(mNode.c_str(), mService.c_str(), &hints, &result) == 0) {
		for (auto p = result; p; p = p->ai_next) {
			if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
				char nodebuffer[MAX_NUMERICNODE_LEN];
				char servbuffer[MAX_NUMERICSERV_LEN];
				if (getnameinfo(p->ai_addr, socklen_t(p->ai_addrlen), nodebuffer,
				                MAX_NUMERICNODE_LEN, servbuffer, MAX_NUMERICSERV_LEN,
				                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {

					mAddress = nodebuffer;
					mPort = uint16_t(std::stoul(servbuffer));
					mFamily = p->ai_family == AF_INET6 ? Family::Ipv6 : Family::Ipv4;
					PLOG_VERBOSE << "Resolved candidate: " << mAddress << ' ' << mPort;
					break;
				}
			}
		}

		freeaddrinfo(result);
	}

	return mFamily != Family::Unresolved;
}

Candidate::Type Candidate::type() const { return mType; }

Candidate::TransportType Candidate::transportType() const { return mTransportType; }

uint32_t Candidate::priority() const { return mPriority; }

string Candidate::candidate() const {
	const char sp{' '};
	std::ostringstream oss;
	oss << "candidate:";
	oss << mFoundation << sp << mComponent << sp << mTransportString << sp << mPriority << sp;
	if (isResolved())
		oss << mAddress << sp << mPort;
	else
		oss << mNode << sp << mService;

	oss << sp << "typ" << sp << mTypeString;

	if (!mTail.empty())
		oss << sp << mTail;

	return oss.str();
}

string Candidate::mid() const { return mMid.value_or("0"); }

Candidate::operator string() const {
	std::ostringstream line;
	line << "a=" << candidate();
	return line.str();
}

bool Candidate::operator==(const Candidate &other) const {
	return mFoundation == other.mFoundation;
}

bool Candidate::operator!=(const Candidate &other) const {
	return mFoundation != other.mFoundation;
}

bool Candidate::isResolved() const { return mFamily != Family::Unresolved; }

Candidate::Family Candidate::family() const { return mFamily; }

std::optional<string> Candidate::address() const {
	return isResolved() ? std::make_optional(mAddress) : nullopt;
}

std::optional<uint16_t> Candidate::port() const {
	return isResolved() ? std::make_optional(mPort) : nullopt;
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
		return out << "prflx";
	case rtc::Candidate::Type::ServerReflexive:
		return out << "srflx";
	case rtc::Candidate::Type::Relayed:
		return out << "relay";
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
