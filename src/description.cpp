/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2020 Staz Modrzynski
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

#include "impl/internals.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <unordered_map>

using std::chrono::system_clock;

namespace {

using std::string;
using std::string_view;

inline bool match_prefix(string_view str, string_view prefix) {
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

inline std::pair<string_view, string_view> parse_pair(string_view attr) {
	string_view key, value;
	if (size_t separator = attr.find(':'); separator != string::npos) {
		key = attr.substr(0, separator);
		value = attr.substr(separator + 1);
	} else {
		key = attr;
	}
	return std::make_pair(std::move(key), std::move(value));
}

template <typename T> T to_integer(string_view s) {
	const string str(s);
	try {
		return std::is_signed<T>::value ? T(std::stol(str)) : T(std::stoul(str));
	} catch (...) {
		throw std::invalid_argument("Invalid integer \"" + str + "\" in description");
	}
}

inline bool is_sha256_fingerprint(string_view f) {
	if (f.size() != 32 * 3 - 1)
		return false;

	for (size_t i = 0; i < f.size(); ++i) {
		if (i % 3 == 2) {
			if (f[i] != ':')
				return false;
		} else {
			if (!std::isxdigit(f[i]))
				return false;
		}
	}
	return true;
}

} // namespace

namespace rtc {

Description::Description(const string &sdp, Type type, Role role)
    : mType(Type::Unspec), mRole(role) {
	hintType(type);

	int index = -1;
	shared_ptr<Entry> current;
	std::istringstream ss(sdp);
	while (ss) {
		string line;
		std::getline(ss, line);
		trim_end(line);
		if (line.empty())
			continue;

		if (match_prefix(line, "m=")) { // Media description line (aka m-line)
			current = createEntry(line.substr(2), std::to_string(++index), Direction::Unknown);

		} else if (match_prefix(line, "o=")) { // Origin line
			std::istringstream origin(line.substr(2));
			origin >> mUsername >> mSessionId;

		} else if (match_prefix(line, "a=")) { // Attribute line
			string attr = line.substr(2);
			auto [key, value] = parse_pair(attr);

			if (key == "setup") {
				if (value == "active")
					mRole = Role::Active;
				else if (value == "passive")
					mRole = Role::Passive;
				else
					mRole = Role::ActPass;

			} else if (key == "fingerprint") {
				if (match_prefix(value, "sha-256 ")) {
					string fingerprint{value.substr(8)};
					trim_begin(fingerprint);
					setFingerprint(std::move(fingerprint));
				} else {
					PLOG_WARNING << "Unknown SDP fingerprint format: " << value;
				}
			} else if (key == "ice-ufrag") {
				mIceUfrag = value;
			} else if (key == "ice-pwd") {
				mIcePwd = value;
			} else if (key == "candidate") {
				addCandidate(Candidate(attr, bundleMid()));
			} else if (key == "end-of-candidates") {
				mEnded = true;
			} else if (current) {
				current->parseSdpLine(std::move(line));
			}

		} else if (current) {
			current->parseSdpLine(std::move(line));
		}
	}

	if (mUsername.empty())
		mUsername = "rtc";

	if (mSessionId.empty()) {
		auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
		std::default_random_engine generator(seed);
		std::uniform_int_distribution<uint32_t> uniform;
		mSessionId = std::to_string(uniform(generator));
	}
}

Description::Description(const string &sdp, string typeString)
    : Description(sdp, !typeString.empty() ? stringToType(typeString) : Type::Unspec,
                  Role::ActPass) {}

Description::Type Description::type() const { return mType; }

string Description::typeString() const { return typeToString(mType); }

Description::Role Description::role() const { return mRole; }

string Description::bundleMid() const {
	// Get the mid of the first media
	return !mEntries.empty() ? mEntries[0]->mid() : "0";
}

optional<string> Description::iceUfrag() const { return mIceUfrag; }

optional<string> Description::icePwd() const { return mIcePwd; }

optional<string> Description::fingerprint() const { return mFingerprint; }

bool Description::ended() const { return mEnded; }

void Description::hintType(Type type) {
	if (mType == Type::Unspec) {
		mType = type;
		if (mType == Type::Answer && mRole == Role::ActPass)
			mRole = Role::Passive; // ActPass is illegal for an answer, so default to passive
	}
}

void Description::setFingerprint(string fingerprint) {
	if (!is_sha256_fingerprint(fingerprint))
		throw std::invalid_argument("Invalid SHA256 fingerprint \"" + fingerprint + "\"");

	std::transform(fingerprint.begin(), fingerprint.end(), fingerprint.begin(),
	               [](char c) { return char(std::toupper(c)); });
	mFingerprint.emplace(std::move(fingerprint));
}

bool Description::hasCandidate(const Candidate &candidate) const {
	for (const Candidate &other : mCandidates)
		if (candidate == other)
			return true;

	return false;
}

void Description::addCandidate(Candidate candidate) {
	candidate.hintMid(bundleMid());

	for (const Candidate &other : mCandidates)
		if (candidate == other)
			return;

	mCandidates.emplace_back(std::move(candidate));
}

void Description::addCandidates(std::vector<Candidate> candidates) {
	for (Candidate candidate : candidates)
		addCandidate(std::move(candidate));
}

void Description::endCandidates() { mEnded = true; }

std::vector<Candidate> Description::extractCandidates() {
	std::vector<Candidate> result;
	std::swap(mCandidates, result);
	mEnded = false;
	return result;
}

Description::operator string() const { return generateSdp("\r\n"); }

string Description::generateSdp(string_view eol) const {
	std::ostringstream sdp;

	// Header
	sdp << "v=0" << eol;
	sdp << "o=" << mUsername << " " << mSessionId << " 0 IN IP4 127.0.0.1" << eol;
	sdp << "s=-" << eol;
	sdp << "t=0 0" << eol;

	// Bundle (RFC8843 Negotiating Media Multiplexing Using the Session Description Protocol)
	// https://tools.ietf.org/html/rfc8843
	sdp << "a=group:BUNDLE";
	for (const auto &entry : mEntries)
		sdp << ' ' << entry->mid();
	sdp << eol;

	// Lip-sync
	std::ostringstream lsGroup;
	for (const auto &entry : mEntries)
		if (entry != mApplication)
			lsGroup << ' ' << entry->mid();

	if (!lsGroup.str().empty())
		sdp << "a=group:LS" << lsGroup.str() << eol;

	// Session-level attributes
	sdp << "a=msid-semantic:WMS *" << eol;
	sdp << "a=setup:" << mRole << eol;

	if (mIceUfrag)
		sdp << "a=ice-ufrag:" << *mIceUfrag << eol;
	if (mIcePwd)
		sdp << "a=ice-pwd:" << *mIcePwd << eol;
	if (!mEnded)
		sdp << "a=ice-options:trickle" << eol;
	if (mFingerprint)
		sdp << "a=fingerprint:sha-256 " << *mFingerprint << eol;

	auto cand = defaultCandidate();
	const string addr = cand && cand->isResolved()
	                        ? (string(cand->family() == Candidate::Family::Ipv6 ? "IP6" : "IP4") +
	                           " " + *cand->address())
	                        : "IP4 0.0.0.0";
	const string port = std::to_string(
	    cand && cand->isResolved() ? *cand->port() : 9); // Port 9 is the discard protocol

	// Entries
	bool first = true;
	for (const auto &entry : mEntries) {
		sdp << entry->generateSdp(eol, addr, port);

		if (std::exchange(first, false)) {
			// Candidates
			for (const auto &candidate : mCandidates)
				sdp << string(candidate) << eol;

			if (mEnded)
				sdp << "a=end-of-candidates" << eol;
		}
	}

	return sdp.str();
}

string Description::generateApplicationSdp(string_view eol) const {
	std::ostringstream sdp;

	// Header
	sdp << "v=0" << eol;
	sdp << "o=" << mUsername << " " << mSessionId << " 0 IN IP4 127.0.0.1" << eol;
	sdp << "s=-" << eol;
	sdp << "t=0 0" << eol;

	auto cand = defaultCandidate();
	const string addr = cand && cand->isResolved()
	                        ? (string(cand->family() == Candidate::Family::Ipv6 ? "IP6" : "IP4") +
	                           " " + *cand->address())
	                        : "IP4 0.0.0.0";
	const string port = std::to_string(
	    cand && cand->isResolved() ? *cand->port() : 9); // Port 9 is the discard protocol

	// Application
	auto app = mApplication ? mApplication : std::make_shared<Application>();
	sdp << app->generateSdp(eol, addr, port);

	// Session-level attributes
	sdp << "a=msid-semantic:WMS *" << eol;
	sdp << "a=setup:" << mRole << eol;

	if (mIceUfrag)
		sdp << "a=ice-ufrag:" << *mIceUfrag << eol;
	if (mIcePwd)
		sdp << "a=ice-pwd:" << *mIcePwd << eol;
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

optional<Candidate> Description::defaultCandidate() const {
	// Return the first host candidate with highest priority, favoring IPv4
	optional<Candidate> result;
	for (const auto &c : mCandidates) {
		if (c.type() == Candidate::Type::Host) {
			if (!result ||
			    (result->family() == Candidate::Family::Ipv6 &&
			     c.family() == Candidate::Family::Ipv4) ||
			    (result->family() == c.family() && result->priority() < c.priority()))
				result.emplace(c);
		}
	}
	return result;
}

shared_ptr<Description::Entry> Description::createEntry(string mline, string mid, Direction dir) {
	string type = mline.substr(0, mline.find(' '));
	if (type == "application") {
		removeApplication();
		mApplication = std::make_shared<Application>(std::move(mid));
		mEntries.emplace_back(mApplication);
		return mApplication;
	} else {
		auto media = std::make_shared<Media>(std::move(mline), std::move(mid), dir);
		mEntries.emplace_back(media);
		return media;
	}
}

void Description::removeApplication() {
	if (!mApplication)
		return;

	auto it = std::find(mEntries.begin(), mEntries.end(), mApplication);
	if (it != mEntries.end())
		mEntries.erase(it);

	mApplication.reset();
}

bool Description::hasApplication() const { return mApplication != nullptr; }

bool Description::hasAudioOrVideo() const {
	for (auto entry : mEntries)
		if (entry != mApplication)
			return true;

	return false;
}

bool Description::hasMid(string_view mid) const {
	for (const auto &entry : mEntries)
		if (entry->mid() == mid)
			return true;

	return false;
}

int Description::addMedia(Media media) {
	mEntries.emplace_back(std::make_shared<Media>(std::move(media)));
	return int(mEntries.size()) - 1;
}

int Description::addMedia(Application application) {
	removeApplication();
	mApplication = std::make_shared<Application>(std::move(application));
	mEntries.emplace_back(mApplication);
	return int(mEntries.size()) - 1;
}

int Description::addApplication(string mid) { return addMedia(Application(std::move(mid))); }

const Description::Application *Description::application() const { return mApplication.get(); }

Description::Application *Description::application() { return mApplication.get(); }

int Description::addVideo(string mid, Direction dir) {
	return addMedia(Video(std::move(mid), dir));
}

int Description::addAudio(string mid, Direction dir) {
	return addMedia(Audio(std::move(mid), dir));
}

void Description::clearMedia() {
	mEntries.clear();
	mApplication.reset();
}

variant<Description::Media *, Description::Application *> Description::media(unsigned int index) {
	if (index >= mEntries.size())
		throw std::out_of_range("Media index out of range");

	const auto &entry = mEntries[index];
	if (entry == mApplication) {
		auto result = dynamic_cast<Application *>(entry.get());
		if (!result)
			throw std::logic_error("Bad type of application in description");
		return result;
	} else {
		auto result = dynamic_cast<Media *>(entry.get());
		if (!result)
			throw std::logic_error("Bad type of media in description");
		return result;
	}
}

variant<const Description::Media *, const Description::Application *>
Description::media(unsigned int index) const {
	if (index >= mEntries.size())
		throw std::out_of_range("Media index out of range");

	const auto &entry = mEntries[index];
	if (entry == mApplication) {
		auto result = dynamic_cast<Application *>(entry.get());
		if (!result)
			throw std::logic_error("Bad type of application in description");
		return result;
	} else {
		auto result = dynamic_cast<Media *>(entry.get());
		if (!result)
			throw std::logic_error("Bad type of media in description");
		return result;
	}
}

unsigned int Description::mediaCount() const { return unsigned(mEntries.size()); }

Description::Entry::Entry(const string &mline, string mid, Direction dir)
    : mMid(std::move(mid)), mDirection(dir) {

	unsigned int port;
	std::istringstream ss(mline);
	ss >> mType;
	ss >> port; // ignored
	ss >> mDescription;
}

void Description::Entry::setDirection(Direction dir) { mDirection = dir; }

Description::Entry::operator string() const { return generateSdp("\r\n", "IP4 0.0.0.0", "9"); }

string Description::Entry::generateSdp(string_view eol, string_view addr, string_view port) const {
	std::ostringstream sdp;
	sdp << "m=" << type() << ' ' << port << ' ' << description() << eol;
	sdp << "c=IN " << addr << eol;
	sdp << generateSdpLines(eol);

	return sdp.str();
}

string Description::Entry::generateSdpLines(string_view eol) const {
	std::ostringstream sdp;
	sdp << "a=bundle-only" << eol;
	sdp << "a=mid:" << mMid << eol;

	for (auto it = mExtMap.begin(); it != mExtMap.end(); ++it) {
		auto &map = it->second;

		sdp << "a=extmap:" << map.id;
		switch (map.direction) {
		case Direction::SendOnly:
			sdp << "/sendonly";
			break;
		case Direction::RecvOnly:
			sdp << "/recvonly";
			break;
		case Direction::SendRecv:
			sdp << "/sendrecv";
			break;
		case Direction::Inactive:
			sdp << "/inactive";
			break;
		default:
			// Ignore
			break;
		}
		sdp << ' ' << map.uri;
		if (!map.attributes.empty())
			sdp << ' ' << map.attributes;
		sdp << eol;
	}

	switch (mDirection) {
	case Direction::SendOnly:
		sdp << "a=sendonly" << eol;
		break;
	case Direction::RecvOnly:
		sdp << "a=recvonly" << eol;
		break;
	case Direction::SendRecv:
		sdp << "a=sendrecv" << eol;
		break;
	case Direction::Inactive:
		sdp << "a=inactive" << eol;
		break;
	default:
		// Ignore
		break;
	}

	for (const auto &attr : mAttributes) {
		if (attr.find("extmap") == string::npos && attr.find("rtcp-rsize") == string::npos)
			sdp << "a=" << attr << eol;
	}

	return sdp.str();
}

void Description::Entry::parseSdpLine(string_view line) {
	if (match_prefix(line, "a=")) {
		string_view attr = line.substr(2);
		auto [key, value] = parse_pair(attr);

		if (key == "mid")
			mMid = value;
		else if (key == "extmap") {
			auto id = Description::Media::ExtMap::parseId(value);
			auto it = mExtMap.find(id);
			if (it == mExtMap.end()) {
				it = mExtMap.insert(std::make_pair(id, Description::Media::ExtMap(value))).first;
			} else {
				it->second.setDescription(value);
			}
		} else if (attr == "sendonly")
			mDirection = Direction::SendOnly;
		else if (attr == "recvonly")
			mDirection = Direction::RecvOnly;
		else if (key == "sendrecv")
			mDirection = Direction::SendRecv;
		else if (key == "inactive")
			mDirection = Direction::Inactive;
		else if (key == "bundle-only") {
			// always added
		} else
			mAttributes.emplace_back(line.substr(2));
	}
}

std::vector<string>::iterator Description::Entry::beginAttributes() { return mAttributes.begin(); }

std::vector<string>::iterator Description::Entry::endAttributes() { return mAttributes.end(); }

std::vector<string>::iterator
Description::Entry::removeAttribute(std::vector<string>::iterator it) {
	return mAttributes.erase(it);
}

void Description::Entry::addExtMap(const Description::Entry::ExtMap &map) {
	mExtMap.emplace(map.id, map);
}

std::map<int, Description::Entry::ExtMap>::iterator Description::Entry::beginExtMaps() {
	return mExtMap.begin();
}

std::map<int, Description::Entry::ExtMap>::iterator Description::Entry::endExtMaps() {
	return mExtMap.end();
}

std::map<int, Description::Entry::ExtMap>::iterator
Description::Entry::removeExtMap(std::map<int, Description::Entry::ExtMap>::iterator iterator) {
	return mExtMap.erase(iterator);
}

Description::Entry::ExtMap::ExtMap(string_view description) { setDescription(description); }

int Description::Entry::ExtMap::parseId(string_view view) {
	size_t p = view.find(' ');

	return to_integer<int>(view.substr(0, p));
}

void Description::Entry::ExtMap::setDescription(string_view description) {
	const size_t uriStart = description.find(' ');
	if (uriStart == string::npos)
		throw std::invalid_argument("Invalid description");

	const string_view idAndDirection = description.substr(0, uriStart);
	const size_t idSplit = idAndDirection.find('/');
	if (idSplit == string::npos) {
		this->id = to_integer<int>(idAndDirection);
	} else {
		this->id = to_integer<int>(idAndDirection.substr(0, idSplit));

		const string_view directionStr = idAndDirection.substr(idSplit + 1);
		if (directionStr == "sendonly")
			this->direction = Direction::SendOnly;
		else if (directionStr == "recvonly")
			this->direction = Direction::RecvOnly;
		else if (directionStr == "sendrecv")
			this->direction = Direction::SendRecv;
		else if (directionStr == "inactive")
			this->direction = Direction::Inactive;
		else
			throw std::invalid_argument("Invalid direction");
	}

	const string_view uriAndAttributes = description.substr(uriStart + 1);
	const size_t attributeSplit = uriAndAttributes.find(' ');

	if (attributeSplit == string::npos)
		this->uri = uriAndAttributes;
	else {
		this->uri = uriAndAttributes.substr(0, attributeSplit);
		this->attributes = uriAndAttributes.substr(attributeSplit + 1);
	}
}

void Description::Media::addSSRC(uint32_t ssrc, optional<string> name, optional<string> msid,
                                 optional<string> trackID) {
	if (name) {
		mAttributes.emplace_back("ssrc:" + std::to_string(ssrc) + " cname:" + *name);
		mCNameMap.emplace(ssrc, *name);
	} else {
		mAttributes.emplace_back("ssrc:" + std::to_string(ssrc));
	}

	if (msid)
		mAttributes.emplace_back("ssrc:" + std::to_string(ssrc) + " msid:" + *msid + " " +
		                         trackID.value_or(*msid));

	mSsrcs.emplace_back(ssrc);
}

void Description::Media::removeSSRC(uint32_t oldSSRC) {
	auto it = mAttributes.begin();
	while (it != mAttributes.end()) {
		if (match_prefix(*it, "ssrc:" + std::to_string(oldSSRC)))
			it = mAttributes.erase(it);
		else
			++it;
	}

	auto jt = mSsrcs.begin();
	while (jt != mSsrcs.end()) {
		if (*jt == oldSSRC)
			jt = mSsrcs.erase(jt);
		else
			++jt;
	}
}

void Description::Media::replaceSSRC(uint32_t oldSSRC, uint32_t ssrc, optional<string> name,
                                     optional<string> msid, optional<string> trackID) {
	removeSSRC(oldSSRC);
	addSSRC(ssrc, std::move(name), std::move(msid), std::move(trackID));
}

bool Description::Media::hasSSRC(uint32_t ssrc) {
	return std::find(mSsrcs.begin(), mSsrcs.end(), ssrc) != mSsrcs.end();
}

Description::Application::Application(string mid)
    : Entry("application 9 UDP/DTLS/SCTP", std::move(mid), Direction::SendRecv) {}

string Description::Application::description() const {
	return Entry::description() + " webrtc-datachannel";
}

Description::Application Description::Application::reciprocate() const {
	Application reciprocated(*this);

	reciprocated.mMaxMessageSize.reset();

	return reciprocated;
}

string Description::Application::generateSdpLines(string_view eol) const {
	std::ostringstream sdp;
	sdp << Entry::generateSdpLines(eol);

	if (mSctpPort)
		sdp << "a=sctp-port:" << *mSctpPort << eol;

	if (mMaxMessageSize)
		sdp << "a=max-message-size:" << *mMaxMessageSize << eol;

	return sdp.str();
}

void Description::Application::parseSdpLine(string_view line) {
	if (match_prefix(line, "a=")) {
		string_view attr = line.substr(2);
		auto [key, value] = parse_pair(attr);

		if (key == "sctp-port") {
			mSctpPort = to_integer<uint16_t>(value);
		} else if (key == "max-message-size") {
			mMaxMessageSize = to_integer<size_t>(value);
		} else {
			Entry::parseSdpLine(line);
		}
	} else {
		Entry::parseSdpLine(line);
	}
}

Description::Media::Media(const string &sdp) : Entry(sdp, "", Direction::Unknown) {
	std::istringstream ss(sdp);
	while (ss) {
		string line;
		std::getline(ss, line);
		trim_end(line);
		if (line.empty())
			continue;

		parseSdpLine(line);
	}

	if (mid().empty())
		throw std::invalid_argument("Missing mid in media SDP");
}

Description::Media::Media(const string &mline, string mid, Direction dir)
    : Entry(mline, std::move(mid), dir) {}

string Description::Media::description() const {
	std::ostringstream desc;
	desc << Entry::description();
	for (auto it = mRtpMap.begin(); it != mRtpMap.end(); ++it)
		desc << ' ' << it->first;

	return desc.str();
}

Description::Media Description::Media::reciprocate() const {
	Media reciprocated(*this);

	// Invert direction
	switch (reciprocated.direction()) {
	case Direction::RecvOnly:
		reciprocated.setDirection(Direction::SendOnly);
		break;
	case Direction::SendOnly:
		reciprocated.setDirection(Direction::RecvOnly);
		break;
	default:
		// We are good
		break;
	}

	// Invert directions of extmap
	for (auto it = reciprocated.mExtMap.begin(); it != reciprocated.mExtMap.end(); ++it) {
		auto &map = it->second;
		switch (map.direction) {
		case Direction::RecvOnly:
			map.direction = Direction::SendOnly;
			break;
		case Direction::SendOnly:
			map.direction = Direction::RecvOnly;
			break;
		default:
			// We are good
			break;
		}
	}

	// Clear all ssrc attributes as they are individual
	auto it = reciprocated.mAttributes.begin();
	while (it != reciprocated.mAttributes.end()) {
		if (match_prefix(*it, "ssrc:"))
			it = reciprocated.mAttributes.erase(it);
		else
			++it;
	}
	reciprocated.mSsrcs.clear();
	reciprocated.mCNameMap.clear();

	return reciprocated;
}

Description::Media::RTPMap &Description::Media::getFormat(int fmt) {
	auto it = mRtpMap.find(fmt);
	if (it != mRtpMap.end())
		return it->second;

	throw std::invalid_argument("m-line index is out of bounds");
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
						std::cout << *it2 << ' ' << remid << std::endl;
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

void Description::Video::addVideoCodec(int payloadType, string codec, optional<string> profile) {
	RTPMap map(std::to_string(payloadType) + ' ' + codec + "/90000");
	map.addFB("nack");
	map.addFB("nack pli");
	//    map.addFB("ccm fir");
	map.addFB("goog-remb");
	if (profile)
		map.fmtps.emplace_back(*profile);
	addRTPMap(map);

	/* TODO
	 *  TIL that Firefox does not properly support the negotiation of RTX! It works, but doesn't
	 * negotiate the SSRC so we have no idea what SSRC is RTX going to be. Three solutions: One) we
	 * don't negotitate it and (maybe) break RTX support with Edge. Two) we do negotiate it and
	 * rebuild the original packet before we send it distribute it to each track. Three) we complain
	 * to mozilla. This one probably won't do much.
	 */
	// RTX Packets
	// RTPMap rtx(std::to_string(payloadType+1) + " rtx/90000");
	// // TODO rtx-time is how long can a request be stashed for before needing to resend it.
	// Needs to be parameterized rtx.addAttribute("apt=" + std::to_string(payloadType) +
	// ";rtx-time=3000"); addRTPMap(rtx);
}

void Description::Audio::addAudioCodec(int payloadType, string codec, optional<string> profile) {
	// TODO This 48000/2 should be parameterized
	RTPMap map(std::to_string(payloadType) + ' ' + codec + "/48000/2");
	if (profile)
		map.fmtps.emplace_back(*profile);
	addRTPMap(map);
}

void Description::Media::addRTXCodec(unsigned int payloadType, unsigned int originalPayloadType,
                                     unsigned int clockRate) {
	RTPMap map(std::to_string(payloadType) + " RTX/" + std::to_string(clockRate));
	map.fmtps.emplace_back("apt=" + std::to_string(originalPayloadType));
	addRTPMap(map);
}

void Description::Video::addH264Codec(int pt, optional<string> profile) {
	addVideoCodec(pt, "H264", profile);
}

void Description::Video::addVP8Codec(int payloadType) {
	addVideoCodec(payloadType, "VP8", nullopt);
}

void Description::Video::addVP9Codec(int payloadType) {
	addVideoCodec(payloadType, "VP9", nullopt);
}

void Description::Media::setBitrate(int bitrate) { mBas = bitrate; }

int Description::Media::getBitrate() const { return mBas; }

bool Description::Media::hasPayloadType(int payloadType) const {
	return mRtpMap.find(payloadType) != mRtpMap.end();
}

string Description::Media::generateSdpLines(string_view eol) const {
	std::ostringstream sdp;
	if (mBas >= 0)
		sdp << "b=AS:" << mBas << eol;

	sdp << Entry::generateSdpLines(eol);
	sdp << "a=rtcp-mux" << eol;

	for (auto it = mRtpMap.begin(); it != mRtpMap.end(); ++it) {
		auto &map = it->second;

		// Create the a=rtpmap
		sdp << "a=rtpmap:" << map.pt << ' ' << map.format << '/' << map.clockRate;
		if (!map.encParams.empty())
			sdp << '/' << map.encParams;
		sdp << eol;

		for (const auto &val : map.rtcpFbs) {
			if (val != "transport-cc")
				sdp << "a=rtcp-fb:" << map.pt << ' ' << val << eol;
		}
		for (const auto &val : map.fmtps)
			sdp << "a=fmtp:" << map.pt << ' ' << val << eol;
	}

	return sdp.str();
}

void Description::Media::parseSdpLine(string_view line) {
	if (match_prefix(line, "a=")) {
		string_view attr = line.substr(2);
		auto [key, value] = parse_pair(attr);

		if (key == "rtpmap") {
			auto pt = Description::Media::RTPMap::parsePT(value);
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end()) {
				it = mRtpMap.insert(std::make_pair(pt, Description::Media::RTPMap(value))).first;
			} else {
				it->second.setMLine(value);
			}
		} else if (key == "rtcp-fb") {
			size_t p = value.find(' ');
			int pt = to_integer<int>(value.substr(0, p));
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end()) {
				it = mRtpMap.insert(std::make_pair(pt, Description::Media::RTPMap())).first;
			}
			it->second.rtcpFbs.emplace_back(value.substr(p + 1));
		} else if (key == "fmtp") {
			size_t p = value.find(' ');
			int pt = to_integer<int>(value.substr(0, p));
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end())
				it = mRtpMap.insert(std::make_pair(pt, Description::Media::RTPMap())).first;
			it->second.fmtps.emplace_back(value.substr(p + 1));
		} else if (key == "rtcp-mux") {
			// always added
		} else if (key == "ssrc") {
			auto ssrc = to_integer<uint32_t>(value);
			if (!hasSSRC(ssrc)) {
				mSsrcs.emplace_back(ssrc);
			}
			auto cnamePos = value.find("cname:");
			if (cnamePos != string::npos) {
				auto cname = value.substr(cnamePos + 6);
				mCNameMap.emplace(ssrc, cname);
			}
			mAttributes.emplace_back(attr);
		} else {
			Entry::parseSdpLine(line);
		}
	} else if (match_prefix(line, "b=AS")) {
		mBas = to_integer<int>(line.substr(line.find(':') + 1));
	} else {
		Entry::parseSdpLine(line);
	}
}

void Description::Media::addRTPMap(const Description::Media::RTPMap &map) {
	mRtpMap.emplace(map.pt, map);
}

std::vector<uint32_t> Description::Media::getSSRCs() { return mSsrcs; }

optional<string> Description::Media::getCNameForSsrc(uint32_t ssrc) {
	auto it = mCNameMap.find(ssrc);
	if (it != mCNameMap.end()) {
		return it->second;
	}
	return nullopt;
}

std::map<int, Description::Media::RTPMap>::iterator Description::Media::beginMaps() {
	return mRtpMap.begin();
}

std::map<int, Description::Media::RTPMap>::iterator Description::Media::endMaps() {
	return mRtpMap.end();
}

std::map<int, Description::Media::RTPMap>::iterator
Description::Media::removeMap(std::map<int, Description::Media::RTPMap>::iterator iterator) {
	return mRtpMap.erase(iterator);
}

Description::Media::RTPMap::RTPMap(string_view mline) { setMLine(mline); }

void Description::Media::RTPMap::removeFB(const string &str) {
	auto it = rtcpFbs.begin();
	while (it != rtcpFbs.end()) {
		if (it->find(str) != string::npos) {
			it = rtcpFbs.erase(it);
		} else
			it++;
	}
}

void Description::Media::RTPMap::addFB(const string &str) { rtcpFbs.emplace_back(str); }

int Description::Media::RTPMap::parsePT(string_view view) {
	size_t p = view.find(' ');

	return to_integer<int>(view.substr(0, p));
}

void Description::Media::RTPMap::setMLine(string_view mline) {
	size_t p = mline.find(' ');
	if (p == string::npos)
		throw std::invalid_argument("Invalid m-line");

	this->pt = to_integer<int>(mline.substr(0, p));

	string_view line = mline.substr(p + 1);
	size_t spl = line.find('/');
	if (spl == string::npos)
		throw std::invalid_argument("Invalid m-line");

	this->format = line.substr(0, spl);

	line = line.substr(spl + 1);
	spl = line.find('/');
	if (spl == string::npos) {
		spl = line.find(' ');
	}
	if (spl == string::npos)
		this->clockRate = to_integer<int>(line);
	else {
		this->clockRate = to_integer<int>(line.substr(0, spl));
		this->encParams = line.substr(spl + 1);
	}
}

Description::Audio::Audio(string mid, Direction dir)
    : Media("audio 9 UDP/TLS/RTP/SAVPF", std::move(mid), dir) {}

void Description::Audio::addOpusCodec(int payloadType, optional<string> profile) {
	addAudioCodec(payloadType, "OPUS", profile);
}

Description::Video::Video(string mid, Direction dir)
    : Media("video 9 UDP/TLS/RTP/SAVPF", std::move(mid), dir) {}

Description::Type Description::stringToType(const string &typeString) {
	using TypeMap_t = std::unordered_map<string, Type>;
	static const TypeMap_t TypeMap = {{"unspec", Type::Unspec},
	                                  {"offer", Type::Offer},
	                                  {"answer", Type::Answer},
	                                  {"pranswer", Type::Pranswer},
	                                  {"rollback", Type::Rollback}};
	auto it = TypeMap.find(typeString);
	return it != TypeMap.end() ? it->second : Type::Unspec;
}

string Description::typeToString(Type type) {
	switch (type) {
	case Type::Unspec:
		return "unspec";
	case Type::Offer:
		return "offer";
	case Type::Answer:
		return "answer";
	case Type::Pranswer:
		return "pranswer";
	case Type::Rollback:
		return "rollback";
	default:
		return "unknown";
	}
}

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Description &description) {
	return out << std::string(description);
}

std::ostream &operator<<(std::ostream &out, rtc::Description::Type type) {
	return out << rtc::Description::typeToString(type);
}

std::ostream &operator<<(std::ostream &out, rtc::Description::Role role) {
	using Role = rtc::Description::Role;
	// Used for SDP generation, do not change
	switch (role) {
	case Role::Active:
		out << "active";
		break;
	case Role::Passive:
		out << "passive";
		break;
	default:
		out << "actpass";
		break;
	}
	return out;
}
