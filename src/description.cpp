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

#include "description.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>

using std::size_t;
using std::string;

namespace {

inline bool hasprefix(const string &str, const string &prefix) {
	return str.size() >= prefix.size() &&
	       std::mismatch(prefix.begin(), prefix.end(), str.begin()).first == prefix.end();
}

inline void trim_end(string &str) {
	str.erase(
	    std::find_if(str.rbegin(), str.rend(), [](char c) { return !std::isspace(c); }).base(),
	    str.end());
}

} // namespace

namespace rtc {

Description::Description(const string &sdp, const string &typeString)
    : Description(sdp, stringToType(typeString), Description::Role::ActPass) {}

Description::Description(const string &sdp, Type type, Role role)
    : mType(type), mRole(role), mMid("0"), mIceUfrag("0"), mIcePwd("0"), mTrickle(true) {
	if (mType == Type::Answer && mRole == Role::ActPass)
		mRole = Role::Passive; // ActPass is illegal for an answer, so default to passive

	auto seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::default_random_engine generator(seed);
	std::uniform_int_distribution<uint32_t> uniform;
	mSessionId = std::to_string(uniform(generator));

	std::istringstream ss(sdp);
	string line;
	while (std::getline(ss, line)) {
		trim_end(line);
		if (hasprefix(line, "a=setup:")) {
			const string setup = line.substr(line.find(':') + 1);
			if (setup == "active")
				mRole = Role::Active;
			else if (setup == "passive")
				mRole = Role::Passive;
			else
				mRole = Role::ActPass;
		} else if (hasprefix(line, "a=mid:")) {
			mMid = line.substr(line.find(':') + 1);
		} else if (hasprefix(line, "a=fingerprint:sha-256")) {
			mFingerprint = line.substr(line.find(' ') + 1);
			std::transform(mFingerprint->begin(), mFingerprint->end(), mFingerprint->begin(),
						   [](char c) { return std::toupper(c); });
		} else if (hasprefix(line, "a=ice-ufrag")) {
			mIceUfrag = line.substr(line.find(':') + 1);
		} else if (hasprefix(line, "a=ice-pwd")) {
			mIcePwd = line.substr(line.find(':') + 1);
		} else if (hasprefix(line, "a=sctp-port")) {
			mSctpPort = uint16_t(std::stoul(line.substr(line.find(':') + 1)));
		} else if (hasprefix(line, "a=candidate")) {
			mCandidates.emplace_back(Candidate(line, mMid));
		} else if (hasprefix(line, "a=end-of-candidates")) {
			mTrickle = false;
		}
	}
}

Description::Type Description::type() const { return mType; }

string Description::typeString() const { return typeToString(mType); }

Description::Role Description::role() const { return mRole; }

string Description::roleString() const { return roleToString(mRole); }

std::optional<string> Description::fingerprint() const { return mFingerprint; }

std::optional<uint16_t> Description::sctpPort() const { return mSctpPort; }

void Description::setFingerprint(string fingerprint) {
	mFingerprint.emplace(std::move(fingerprint));
}

void Description::setSctpPort(uint16_t port) { mSctpPort.emplace(port); }

void Description::addCandidate(std::optional<Candidate> candidate) {
	if (candidate)
		mCandidates.emplace_back(std::move(*candidate));
	else
		mTrickle = false;
}

Description::operator string() const {
	if (!mFingerprint)
		throw std::logic_error("Fingerprint must be set to generate a SDP");

	std::ostringstream sdp;
	sdp << "v=0\n";
	sdp << "o=- " << mSessionId << " 0 IN IP4 0.0.0.0\n";
	sdp << "s=-\n";
	sdp << "t=0 0\n";
	sdp << "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\n";
	sdp << "c=IN IP4 0.0.0.0\n";
	sdp << "a=ice-ufrag:" << mIceUfrag << "\n";
	sdp << "a=ice-pwd:" << mIcePwd << "\n";
	if (mTrickle)
		sdp << "a=ice-options:trickle\n";
	sdp << "a=mid:" << mMid << "\n";
	sdp << "a=setup:" << roleToString(mRole) << "\n";
	sdp << "a=dtls-id:1\n";
	if (mFingerprint)
		sdp << "a=fingerprint:sha-256 " << *mFingerprint << "\n";
	if (mSctpPort)
		sdp << "a=sctp-port:" << *mSctpPort << "\n";

	for (const auto &candidate : mCandidates) {
		sdp << string(candidate) << "\n";
	}

	if (!mTrickle)
		sdp << "a=end-of-candidates\n";

	return sdp.str();
}

Description::Type Description::stringToType(const string &typeString) {
	if (typeString == "offer")
		return Type::Offer;
	else if (typeString == "answer")
		return Type::Answer;
	else
		return Type::Unspec;
}

string Description::typeToString(Type type) {
	switch (type) {
	case Type::Offer:
		return "offer";
	case Type::Answer:
		return "answer";
	default:
		return "";
	}
}

string Description::roleToString(Role role) {
	switch (role) {
	case Role::Active:
		return "active";
	case Role::Passive:
		return "passive";
	default:
		return "actpass";
	}
}

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Description &description) {
	return out << std::string(description);
}

