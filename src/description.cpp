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
using std::chrono::system_clock;

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
    : mType(Type::Unspec), mRole(role) {
	mData.mid = "data";
	hintType(type);

	auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
	std::default_random_engine generator(seed);
	std::uniform_int_distribution<uint32_t> uniform;
	mSessionId = std::to_string(uniform(generator));

	std::istringstream ss(sdp);
	std::optional<Media> currentMedia;

	int mlineIndex = 0;
	bool finished;
	do {
		string line;
		finished = !std::getline(ss, line) && line.empty();
		trim_end(line);

		// Media description line (aka m-line)
		if (finished || match_prefix(line, "m=")) {
			if (currentMedia) {
				if (!currentMedia->mid().empty()) {
					if (currentMedia->type() == "application")
						mData.mid = currentMedia->mid();
					else
						mMedia.emplace(mlineIndex, std::move(*currentMedia));

					++mlineIndex;

				} else if (line.find(" ICE/SDP") != string::npos) {
					PLOG_WARNING << "SDP \"m=\" line has no corresponding mid, ignoring";
				}
			}
			if (!finished)
				currentMedia.emplace(Media(line.substr(2)));

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
					currentMedia->mid() = value;

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
					               mFingerprint->begin(),
					               [](char c) { return char(std::toupper(c)); });
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
				addCandidate(Candidate(attr, currentMedia ? currentMedia->mid() : mData.mid));
			} else if (key == "end-of-candidates") {
				mEnded = true;
			} else if (currentMedia) {
				currentMedia->parseSdpLine(std::move(line));
			}
		} else if (currentMedia) {
			currentMedia->parseSdpLine(std::move(line));
		}
	} while (!finished);
}

Description::Type Description::type() const { return mType; }

string Description::typeString() const { return typeToString(mType); }

Description::Role Description::role() const { return mRole; }

string Description::roleString() const { return roleToString(mRole); }

string Description::dataMid() const { return mData.mid; }

string Description::bundleMid() const {
	// Get the mid of the first media
	if (auto it = mMedia.find(0); it != mMedia.end())
		return it->second.mid();
	else
		return mData.mid;
}

std::optional<string> Description::fingerprint() const { return mFingerprint; }

std::optional<uint16_t> Description::sctpPort() const { return mData.sctpPort; }

std::optional<size_t> Description::maxMessageSize() const { return mData.maxMessageSize; }

bool Description::ended() const { return mEnded; }

void Description::hintType(Type type) {
	if (mType == Type::Unspec) {
		mType = type;
		if (mType == Type::Answer && mRole == Role::ActPass)
			mRole = Role::Passive; // ActPass is illegal for an answer, so default to passive
	}
}

void Description::setDataMid(string mid) { mData.mid = mid; }

void Description::setFingerprint(string fingerprint) {
	mFingerprint.emplace(std::move(fingerprint));
}

void Description::setSctpPort(uint16_t port) { mData.sctpPort.emplace(port); }

void Description::setMaxMessageSize(size_t size) { mData.maxMessageSize.emplace(size); }

void Description::addCandidate(Candidate candidate) {
	mCandidates.emplace_back(std::move(candidate));
}

void Description::endCandidates() { mEnded = true; }

std::vector<Candidate> Description::extractCandidates() {
	std::vector<Candidate> result;
	std::swap(mCandidates, result);
	mEnded = false;
	return result;
}

bool Description::hasMedia() const { return !mMedia.empty(); }

void Description::addMedia(Media media) { mMedia.emplace(int(mMedia.size()), std::move(media)); }

Description::operator string() const { return generateSdp("\r\n"); }

string Description::generateSdp(string_view eol) const {
	std::ostringstream sdp;

	// Header
	sdp << "v=0" << eol;
	sdp << "o=- " << mSessionId << " 0 IN IP4 127.0.0.1" << eol;
	sdp << "s=-" << eol;
	sdp << "t=0 0" << eol;

	// Bundle
	// see Negotiating Media Multiplexing Using the Session Description Protocol
	// https://tools.ietf.org/html/draft-ietf-mmusic-sdp-bundle-negotiation-54
	sdp << "a=group:BUNDLE";
	for (int i = 0; i < int(mMedia.size() + 1); ++i) {
		if (auto it = mMedia.find(i); it != mMedia.end())
			sdp << ' ' << it->second.mid();
		else
			sdp << ' ' << mData.mid;
	}
	sdp << eol;

	// Non-data media
	if (!mMedia.empty()) {
		// Lip-sync
		sdp << "a=group:LS";
		for (const auto &p : mMedia)
			sdp << " " << p.second.mid();
		sdp << eol;
	}

	// Session-level attributes
	sdp << "a=msid-semantic:WMS *" << eol;
	sdp << "a=setup:" << roleToString(mRole) << eol;
	sdp << "a=ice-ufrag:" << mIceUfrag << eol;
	sdp << "a=ice-pwd:" << mIcePwd << eol;

	if (!mEnded)
		sdp << "a=ice-options:trickle" << eol;

	if (mFingerprint)
		sdp << "a=fingerprint:sha-256 " << *mFingerprint << eol;

	// Media descriptions and attributes
	for (int i = 0; i < int(mMedia.size() + 1); ++i) {
		if (auto it = mMedia.find(i); it != mMedia.end()) {
			sdp << it->second.generateSdp(eol);
		} else {
			// Data
			const string description = "UDP/DTLS/SCTP webrtc-datachannel";
			sdp << "m=application" << ' ' << (!mMedia.empty() ? 0 : 9) << ' ' << description << eol;
			sdp << "c=IN IP4 0.0.0.0" << eol;
			if (!mMedia.empty())
				sdp << "a=bundle-only" << eol;
			sdp << "a=mid:" << mData.mid << eol;
			sdp << "a=sendrecv" << eol;
			if (mData.sctpPort)
				sdp << "a=sctp-port:" << *mData.sctpPort << eol;
			if (mData.maxMessageSize)
				sdp << "a=max-message-size:" << *mData.maxMessageSize << eol;
		}
	}

	// Candidates
	for (const auto &candidate : mCandidates)
		sdp << string(candidate) << eol;

	if (mEnded)
		sdp << "a=end-of-candidates" << eol;

	return sdp.str();
}

string Description::generateDataSdp(string_view eol) const {
	std::ostringstream sdp;

	// Header
	sdp << "v=0" << eol;
	sdp << "o=- " << mSessionId << " 0 IN IP4 127.0.0.1" << eol;
	sdp << "s=-" << eol;
	sdp << "t=0 0" << eol;

	// Data
	sdp << "m=application 9 UDP/DTLS/SCTP webrtc-datachannel";
	sdp << "c=IN IP4 0.0.0.0" << eol;
	sdp << "a=mid:" << mData.mid << eol;
	sdp << "a=sendrecv" << eol;
	if (mData.sctpPort)
		sdp << "a=sctp-port:" << *mData.sctpPort << eol;
	if (mData.maxMessageSize)
		sdp << "a=max-message-size:" << *mData.maxMessageSize << eol;

	sdp << "a=setup:" << roleToString(mRole) << eol;
	sdp << "a=ice-ufrag:" << mIceUfrag << eol;
	sdp << "a=ice-pwd:" << mIcePwd << eol;

	if (!mEnded)
		sdp << "a=ice-options:trickle" << eol;

	if (mFingerprint)
		sdp << "a=fingerprint:sha-256 " << *mFingerprint << eol;

	// Candidates
	for (const auto &candidate : mCandidates)
		sdp << string(candidate) << eol;

	if (mEnded)
		sdp << "a=end-of-candidates" << eol;

	return sdp.str();
}

Description::Media::Media(string mline, Direction dir, string mid) {
	size_t p = mline.find(' ');
	mType = mline.substr(0, p);
	if (p != string::npos)
		if (size_t q = mline.find(' ', p + 1); q != string::npos)
			mDescription = mline.substr(q + 1, mline.find(' ', q + 1) - q - 2);

	mMid = mid;
	mAttributes.emplace_back("rtcp-mux");

	setDirection(dir);
}

Description::Media::RTPMap &Description::Media::getFormat(int fmt) {
	auto it = mRtpMap.find(fmt);
	if (it != mRtpMap.end())
		return it->second;

	throw std::invalid_argument("mLineIndex is out of bounds");
}

Description::Media::RTPMap &Description::Media::getFormat(const string &fmt) {
	for (auto it = mRtpMap.begin(); it != mRtpMap.end(); ++it)
		if (it->second.format == fmt)
			return it->second;

	throw std::invalid_argument("format was not found");
}

void Description::Media::removeFormat(const string &fmt) {
	auto it = mRtpMap.begin();
	std::vector<int> remed;

	// Remove the actual formats
	while (it != mRtpMap.end()) {
		if (it->second.format == fmt) {
			remed.emplace_back(it->first);
			it = mRtpMap.erase(it);
		} else {
			it++;
		}
	}

	// Remove any other rtpmaps that depend on the formats we just removed
	it = mRtpMap.begin();
	while (it != mRtpMap.end()) {
		auto it2 = it->second.fmtps.begin();
		bool rem = false;
		while (it2 != it->second.fmtps.end()) {
			if (it2->find("apt=") == 0) {
				for (auto remid : remed) {
					if (it2->find(std::to_string(remid)) != string::npos) {
						std::cout << *it2 << " " << remid << std::endl;
						it = mRtpMap.erase(it);
						rem = true;
						break;
					}
				}
				break;
			}
			it2++;
		}
		if (!rem)
			it++;
	}
}

void Description::Media::addVideoCodec(int payloadType, const string &codec) {
	RTPMap map(std::to_string(payloadType) + " " + codec + "/90000");
	map.addFB("nack");
	map.addFB("goog-remb");
	mRtpMap.emplace(map.pt, map);
}

void Description::Media::addH264Codec(int pt) { addVideoCodec(pt, "H264"); }

void Description::Media::addVP8Codec(int payloadType) { addVideoCodec(payloadType, "VP8"); }

void Description::Media::addVP9Codec(int payloadType) { addVideoCodec(payloadType, "VP9"); }

Description::Direction Description::Media::getDirection() {
	for (auto attr : mAttributes) {
		if (attr == "sendrecv")
			return Direction::SendRecv;
		if (attr == "recvonly")
			return Direction::RecvOnly;
		if (attr == "sendonly")
			return Direction::SendOnly;
	}
	return Direction::Unknown;
}

void Description::Media::setBitrate(int bitrate) { mBas = bitrate; }

int Description::Media::getBitrate() const { return mBas; }

void Description::Media::setDirection(Description::Direction dir) {
	auto it = mAttributes.begin();
	while (it != mAttributes.end()) {
		if (*it == "sendrecv" || *it == "sendonly" || *it == "recvonly")
			it = mAttributes.erase(it);
		else
			it++;
	}
	if (dir == Direction::SendRecv)
		mAttributes.emplace(mAttributes.begin(), "sendrecv");
	else if (dir == Direction::RecvOnly)
		mAttributes.emplace(mAttributes.begin(), "recvonly");
	if (dir == Direction::SendOnly)
		mAttributes.emplace(mAttributes.begin(), "sendonly");
}

string Description::Media::generateSdp(string_view eol) const {
	std::ostringstream sdp;

	sdp << "m=" << mType << ' ' << 0 << ' ' << mDescription;

	for (auto it = mRtpMap.begin(); it != mRtpMap.end(); ++it)
		sdp << ' ' << it->first;

	sdp << eol;
	sdp << "c=IN IP4 0.0.0.0" << eol;
	if (mBas > -1)
		sdp << "b=AS:" << mBas << eol;

	sdp << "a=bundle-only" << eol;
	sdp << "a=mid:" << mMid << eol;

	for (const auto &attr : mAttributes)
		sdp << "a=" << attr << eol;

	for (auto it = mRtpMap.begin(); it != mRtpMap.end(); ++it) {
		auto &map = it->second;

		// Create the a=rtpmap
		sdp << "a=rtpmap:" << map.pt << " " << map.format << "/" << map.clockRate;
		if (!map.encParams.empty())
			sdp << "/" << map.encParams;
		sdp << eol;

		for (const auto &val : map.rtcpFbs)
			sdp << "a=rtcp-fb:" << map.pt << " " << val << eol;
		for (const auto &val : map.fmtps)
			sdp << "a=fmtp:" << map.pt << " " << val << eol;
	}

	for (const auto &attr : mAttributesl)
		sdp << "a=" << attr << eol;

	return sdp.str();
}

void Description::Media::parseSdpLine(string line) {
	if (match_prefix(line, "a=")) {
		string attr = line.substr(2);

		string key, value;
		if (size_t separator = attr.find(':'); separator != string::npos) {
			key = attr.substr(0, separator);
			value = attr.substr(separator + 1);
		} else {
			key = attr;
		}

		if (key == "mid") {
			mMid = value;
		} else if (key == "rtpmap") {
			Description::Media::RTPMap map(value);
			mRtpMap.emplace(map.pt, map);
		} else if (key == "rtcp-fb") {
			size_t p = value.find(' ');
			int pt = std::stoi(value.substr(0, p));
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end()) {
				PLOG_WARNING << "rtcp-fb applied before it's rtpmap. Ignoring";
			} else
				it->second.rtcpFbs.emplace_back(value.substr(p + 1));
		} else if (key == "fmtp") {
			size_t p = value.find(' ');
			int pt = std::stoi(value.substr(0, p));
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end()) {
				PLOG_WARNING << "fmtp applied before it's rtpmap. Ignoring";
			} else {
				it->second.fmtps.emplace_back(value.substr(p + 1));
			}
		} else if (key == "b") {
			// TODO
		} else {
			mAttributes.emplace_back(line.substr(2));
		}
	} else if (match_prefix(line, "b=AS")) {
		mBas = std::stoi(line.substr(line.find(':') + 1));
	}
}

Description::Media::RTPMap::RTPMap(const string &mline) {
	size_t p = mline.find(' ');

	this->pt = std::stoi(mline.substr(0, p));

	auto line = mline.substr(p + 1);
	size_t spl = line.find('/');
	this->format = line.substr(0, spl);

	line = line.substr(spl + 1);
	spl = line.find('/');
	if (spl == string::npos) {
		spl = line.find(' ');
	}
	if (spl == string::npos)
		this->clockRate = std::stoi(line);
	else {
		this->clockRate = std::stoi(line.substr(0, spl));
		this->encParams = line.substr(spl);
	}
}

void Description::Media::RTPMap::removeFB(const string& string) {
	auto it = rtcpFbs.begin();
	while (it != rtcpFbs.end()) {
		if (it->find(string) != std::string::npos) {
			it = rtcpFbs.erase(it);
		} else
			it++;
	}
}

void Description::Media::RTPMap::addFB(const string &string) { rtcpFbs.emplace_back(string); }

Description::AudioMedia::AudioMedia(Direction dir, string mid)
    : Media("audio 9 UDP/TLS/RTP/SAVPF", dir, std::move(mid)) {
}

Description::VideoMedia::VideoMedia(Direction dir, string mid)
    : Media("video 9 UDP/TLS/RTP/SAVPF", dir, std::move(mid)) {}

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

