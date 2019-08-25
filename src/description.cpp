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

Description::Description(Role role, const string &mid) : mRole(role), mMid(mid) {
	auto seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::default_random_engine generator(seed);
	std::uniform_int_distribution<uint32_t> uniform;
	mSessionId = std::to_string(uniform(generator));
}

Description::Description(const string &sdp) {
	std::istringstream ss(sdp);
	string line;
	while (std::getline(ss, line)) {
		trim_end(line);
		if (hasprefix(line, "a=setup:")) {
			const string setup = line.substr(line.find(':') + 1);
			if (setup == "active" && mRole == Role::Active) {
				mRole = Role::Passive;
			} else if (setup == "passive" && mRole == Role::Passive) {
				mRole = Role::Active;
			} else { // actpass, nothing to do
			}
		} else if (hasprefix(line, "a=mid:")) {
			mMid = line.substr(line.find(':') + 1);
		} else if (hasprefix(line, "a=fingerprint:sha-256")) {
			mFingerprint = line.substr(line.find(':') + 1);
		}
	}
}

Description::Role Description::role() const { return mRole; }

void Description::setFingerprint(const string &fingerprint) { mFingerprint = fingerprint; }

void Description::addCandidate(Candidate candidate) {
	mCandidates.emplace_back(std::move(candidate));
}

void Description::addCandidate(Candidate &&candidate) {
	mCandidates.emplace_back(std::forward<Candidate>(candidate));
}

Description::operator string() const {
	if (!mFingerprint)
		throw std::runtime_error("Fingerprint must be set to generate a SDP");

    std::ostringstream sdp;
	sdp << "v=0\r\n";
	sdp << "o=- " << mSessionId << " 0 IN IP4 0.0.0.0\r\n";
	sdp << "s=-\r\n";
	sdp << "t=0 0\r\n";
    sdp << "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
	sdp << "a=ice-options:trickle\r\n";
	sdp << "a=mid:" << mMid << "\r\n";
	sdp << "a=setup:" << (mRole == Role::Active ? "active" : "passive") << "\r\n";
	sdp << "a=dtls-id:1\r\n";
	sdp << "a=fingerprint:sha-256 " << *mFingerprint << "\r\n";
    sdp << "a=sctp-port:5000\r\n";
    // sdp << "a=max-message-size:100000\r\n";

	for (const auto &candidate : mCandidates) {
		sdp << "a=candidate:" << string(candidate);
	}

	return sdp.str();
}

} // namespace rtc

