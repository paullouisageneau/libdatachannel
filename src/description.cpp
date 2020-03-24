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

inline bool match_prefix(const string &str, const string &prefix) {
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
    : Description(sdp, stringToType(typeString)) {}

Description::Description(const string &sdp, Type type) : Description(sdp, type, Role::ActPass) {}

Description::Description(const string &sdp, Type type, Role role)
    : mType(Type::Unspec), mRole(role), mIceUfrag(""), mIcePwd(""), mTrickle(true) {
	mData.mid = "data";
	hintType(type);

	auto seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::default_random_engine generator(seed);
	std::uniform_int_distribution<uint32_t> uniform;
	mSessionId = std::to_string(uniform(generator));

	std::istringstream ss(sdp);
	std::optional<Media> currentMedia;

	string line;
	bool finished;
	do {
		finished = !std::getline(ss, line) && line.empty();
		trim_end(line);

		LOG_DEBUG << line;

		// Media description line (aka m-line)
		if (finished || match_prefix(line, "m=")) {
			if (currentMedia) {
				if (!currentMedia->mid.empty()) {
					if (currentMedia->type() == "application")
						mData.mid = currentMedia->mid;
					else
						mMedia.emplace(currentMedia->mid, std::move(*currentMedia));

				} else if (line.find(" ICE/SDP") != string::npos) {
					PLOG_WARNING << "SDP \"m=\" line has no corresponding mid, ignoring";
				}
			}
			if (!finished)
				currentMedia.emplace(Media{line.substr(2)});

			// Attribute line
		} else if (match_prefix(line, "a=")) {
			string attr = line.substr(2);

			string key, value;
			if (size_t separator = attr.find(':'); separator != string::npos) {
				key = attr.substr(0, separator);
				value = attr.substr(separator + 1);
			} else {
				key = attr;
			}

			if (key == "mid") {
				if (currentMedia)
					currentMedia->mid = value;

			} else if (key == "setup") {
				if (value == "active")
					mRole = Role::Active;
				else if (value == "passive")
					mRole = Role::Passive;
				else
					mRole = Role::ActPass;

			} else if (key == "fingerprint") {
				if (match_prefix(value, "sha-256 ")) {
					mFingerprint = value.substr(8);
					std::transform(mFingerprint->begin(), mFingerprint->end(),
					               mFingerprint->begin(), [](char c) { return std::toupper(c); });
				} else {
					PLOG_WARNING << "Unknown SDP fingerprint type: " << value;
				}
			} else if (key == "ice-ufrag") {
				mIceUfrag = value;
			} else if (key == "ice-pwd") {
				mIcePwd = value;
			} else if (key == "sctp-port") {
				mData.sctpPort = uint16_t(std::stoul(value));
			} else if (key == "max-message-size") {
				mData.maxMessageSize = size_t(std::stoul(value));
			} else if (key == "candidate") {
				addCandidate(Candidate(attr, currentMedia ? currentMedia->mid : mData.mid));
			} else if (key == "end-of-candidates") {
				mTrickle = false;
			} else if (currentMedia) {
				currentMedia->attributes.emplace_back(line.substr(2));
			}
		}
	} while (!finished);
}

Description::Type Description::type() const { return mType; }

string Description::typeString() const { return typeToString(mType); }

Description::Role Description::role() const { return mRole; }

string Description::roleString() const { return roleToString(mRole); }

string Description::dataMid() const { return mData.mid; }

std::optional<string> Description::fingerprint() const { return mFingerprint; }

std::optional<uint16_t> Description::sctpPort() const { return mData.sctpPort; }

std::optional<size_t> Description::maxMessageSize() const { return mData.maxMessageSize; }

bool Description::trickleEnabled() const { return mTrickle; }


void Description::hintType(Type type) {
	if (mType == Type::Unspec) {
		mType = type;
		if (mType == Type::Answer && mRole == Role::ActPass)
			mRole = Role::Passive; // ActPass is illegal for an answer, so default to passive
	}
}

void Description::setFingerprint(string fingerprint) {
	mFingerprint.emplace(std::move(fingerprint));
}

void Description::setSctpPort(uint16_t port) { mData.sctpPort.emplace(port); }

void Description::setMaxMessageSize(size_t size) { mData.maxMessageSize.emplace(size); }

void Description::addCandidate(Candidate candidate) {
	mCandidates.emplace_back(std::move(candidate));
}

void Description::endCandidates() { mTrickle = false; }

std::vector<Candidate> Description::extractCandidates() {
	std::vector<Candidate> result;
	std::swap(mCandidates, result);
	mTrickle = true;
	return result;
}

bool Description::hasMedia() const { return !mMedia.empty(); }

void Description::addMedia(const Description &source) {
	for (auto [mid, media] : source.mMedia)
		if (mid != mData.mid)
			mMedia.emplace(mid, media);
		else
			PLOG_WARNING << "Media mid \"" << mid << "\" is the same as data mid, ignoring";
}

Description::operator string() const { return generateSdp("\r\n"); }

string Description::generateSdp(const string &eol) const {
	if (!mFingerprint)
		throw std::logic_error("Fingerprint must be set to generate an SDP string");

	std::ostringstream sdp;

	// Header
	sdp << "v=0" << eol;
	sdp << "o=- " << mSessionId << " 0 IN IP4 127.0.0.1" << eol;
	sdp << "s=-" << eol;
	sdp << "t=0 0" << eol;

	// Bundle
	sdp << "a=group:BUNDLE";
	for (const auto &[mid, _] : mMedia)
		sdp << " " << mid;
	sdp << " " << mData.mid << eol;

	// Non-data media
	if (!mMedia.empty()) {
		// Lip-sync
		sdp << "a=group:LS";
		for (const auto &[mid, _] : mMedia)
			sdp << " " << mid;
		sdp << eol;

		// Descriptions and attributes
		for (const auto &[_, media] : mMedia) {
			sdp << "m=" << media.description << eol;
			sdp << "c=IN IP4 0.0.0.0" << eol;
			sdp << "a=mid:" << media.mid << eol;
			for (const auto &attr : media.attributes)
				sdp << "a=" << attr << eol;
		}
	}

	// Data
	sdp << "m=application 9 UDP/DTLS/SCTP webrtc-datachannel" << eol;
	sdp << "c=IN IP4 0.0.0.0" << eol;
	sdp << "a=mid:" << mData.mid << eol;
	if (mData.sctpPort)
		sdp << "a=sctp-port:" << *mData.sctpPort << eol;
	if (mData.maxMessageSize)
		sdp << "a=max-message-size:" << *mData.maxMessageSize << eol;

	// Common
	if (mTrickle)
		sdp << "a=ice-options:trickle" << eol;
	sdp << "a=ice-ufrag:" << mIceUfrag << eol;
	sdp << "a=ice-pwd:" << mIcePwd << eol;
	sdp << "a=setup:" << roleToString(mRole) << eol;
	sdp << "a=dtls-id:1" << eol;
	if (mFingerprint)
		sdp << "a=fingerprint:sha-256 " << *mFingerprint << eol;

	// Candidates
	for (const auto &candidate : mCandidates)
		sdp << string(candidate) << eol;
	if (!mTrickle)
		sdp << "a=end-of-candidates" << eol;

	return sdp.str();
}

string Description::Media::type() const { return description.substr(0, description.find(' ')); }

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

