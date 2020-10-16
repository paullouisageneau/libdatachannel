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

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>

using std::shared_ptr;
using std::size_t;
using std::string;
using std::string_view;
using std::chrono::system_clock;

namespace {

inline bool match_prefix(string_view str, string_view prefix) {
	return str.size() >= prefix.size() &&
	       std::mismatch(prefix.begin(), prefix.end(), str.begin()).first == prefix.end();
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
	return std::is_signed<T>::value ? T(std::stol(string(s))) : T(std::stoul(string(s)));
}

} // namespace

namespace rtc {

Description::Description(const string &sdp, const string &typeString)
    : Description(sdp, stringToType(typeString)) {}

Description::Description(const string &sdp, Type type) : Description(sdp, type, Role::ActPass) {}

Description::Description(const string &sdp, Type type, Role role)
    : mType(Type::Unspec), mRole(role) {
	hintType(type);

	auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
	std::default_random_engine generator(seed);
	std::uniform_int_distribution<uint32_t> uniform;
	mSessionId = std::to_string(uniform(generator));

	std::istringstream ss(sdp);
	std::shared_ptr<Entry> current;

	int index = -1;
	string line;
	while (std::getline(ss, line) || !line.empty()) {
		trim_end(line);

		// Media description line (aka m-line)
		if (match_prefix(line, "m=")) {
			++index;
			string mline = line.substr(2);
			current = createEntry(std::move(mline), std::to_string(index), Direction::Unknown);

			// Attribute line
		} else if (match_prefix(line, "a=")) {
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
	};
}

Description::Type Description::type() const { return mType; }

string Description::typeString() const { return typeToString(mType); }

Description::Role Description::role() const { return mRole; }

string Description::roleString() const { return roleToString(mRole); }

string Description::bundleMid() const {
	// Get the mid of the first media
	return !mEntries.empty() ? mEntries[0]->mid() : "0";
}

std::optional<string> Description::fingerprint() const { return mFingerprint; }

bool Description::ended() const { return mEnded; }

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
	sdp << "a=setup:" << roleToString(mRole) << eol;
	sdp << "a=ice-ufrag:" << mIceUfrag << eol;
	sdp << "a=ice-pwd:" << mIcePwd << eol;

	if (!mEnded)
		sdp << "a=ice-options:trickle" << eol;

	if (mFingerprint)
		sdp << "a=fingerprint:sha-256 " << *mFingerprint << eol;

	// Entries
	bool first = true;
	for (const auto &entry : mEntries) {
		sdp << entry->generateSdp(eol);

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
	sdp << "o=- " << mSessionId << " 0 IN IP4 127.0.0.1" << eol;
	sdp << "s=-" << eol;
	sdp << "t=0 0" << eol;

	// Application
	auto app = mApplication ? mApplication : std::make_shared<Application>();
	sdp << app->generateSdp(eol);

	// Session-level attributes
	sdp << "a=msid-semantic:WMS *" << eol;
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

Description::Application *Description::application() { return mApplication.get(); }

int Description::addVideo(string mid, Direction dir) {
	return addMedia(Video(std::move(mid), dir));
}

int Description::addAudio(string mid, Direction dir) {
	return addMedia(Audio(std::move(mid), dir));
}

std::variant<Description::Media *, Description::Application *> Description::media(int index) {
	if (index < 0 || index >= int(mEntries.size()))
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

std::variant<const Description::Media *, const Description::Application *>
Description::media(int index) const {
	if (index < 0 || index >= int(mEntries.size()))
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

int Description::mediaCount() const { return int(mEntries.size()); }

Description::Entry::Entry(const string &mline, string mid, Direction dir)
    : mMid(std::move(mid)), mDirection(dir) {

	unsigned int port;
	std::istringstream ss(mline);
	ss >> mType;
	ss >> port; // ignored
	ss >> mDescription;

}

void Description::Entry::setDirection(Direction dir) { mDirection = dir; }

Description::Entry::operator string() const { return generateSdp("\r\n"); }

string Description::Entry::generateSdp(string_view eol) const {
	std::ostringstream sdp;
	// Port 9 is the discard protocol
	sdp << "m=" << type() << ' ' << 9 << ' ' << description() << eol;
	sdp << "c=IN IP4 0.0.0.0" << eol;
	sdp << generateSdpLines(eol);

	return sdp.str();
}

string Description::Entry::generateSdpLines(string_view eol) const {
	std::ostringstream sdp;
	sdp << "a=bundle-only" << eol;
	sdp << "a=mid:" << mMid << eol;

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

	for (const auto &attr : mAttributes)
		sdp << "a=" << attr << eol;

	return sdp.str();
}

void Description::Entry::parseSdpLine(string_view line) {
	if (match_prefix(line, "a=")) {
		string_view attr = line.substr(2);
		auto [key, value] = parse_pair(attr);

		if (key == "mid")
			mMid = value;
		else if (attr == "sendonly")
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

void Description::Media::addSSRC(uint32_t ssrc, std::string name) {
    mAttributes.emplace_back("ssrc:" + std::to_string(ssrc) + " cname:" + name);
}

bool Description::Media::hasSSRC(uint32_t ssrc) {
    for (auto &val : mAttributes) {
        if (val.find("ssrc:" + std::to_string(ssrc)) != std::string ::npos)
            return true;
    }
    return false;
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
	string line;
	while (std::getline(ss, line) || !line.empty()) {
		trim_end(line);
		parseSdpLine(line);
	}

	if (mid().empty())
		throw std::invalid_argument("Missing mid in media SDP");
}

Description::Media::Media(const string &mline, string mid, Direction dir)
    : Entry(mline, std::move(mid), dir) {
}

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
	switch (direction()) {
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

void Description::Video::addVideoCodec(int payloadType, const string &codec) {
	RTPMap map(std::to_string(payloadType) + ' ' + codec + "/90000");
    map.addFB("nack");
    map.addFB("nack pli");
	map.addFB("goog-remb");
	if (codec == "H264") {
		// Use Constrained Baseline profile Level 4.2 (necessary for Firefox)
		// https://developer.mozilla.org/en-US/docs/Web/Media/Formats/WebRTC_codecs#Supported_video_codecs
		// TODO: Should be 42E0 but 42C0 appears to be more compatible. Investigate this.
		map.fmtps.emplace_back("profile-level-id=42E02A;packetization-mode=1;level-asymmetry-allowed=1");
	}
	addRTPMap(map);

//	// RTX Packets
    RTPMap rtx(std::to_string(payloadType+1) + " RTX/90000");
    // TODO rtx-time is how long can a request be stashed for before needing to resend it. Needs to be parameterized
    rtx.addAttribute("apt=" + std::to_string(payloadType) + ";rtx-time=3000");
    addRTPMap(rtx);
}

void Description::Audio::addAudioCodec(int payloadType, const string &codec) {
    // TODO This 48000/2 should be parameterized
    RTPMap map(std::to_string(payloadType) + ' ' + codec + "/48000/2");
    addRTPMap(map);
}

void Description::Video::addH264Codec(int pt) { addVideoCodec(pt, "H264"); }

void Description::Video::addVP8Codec(int payloadType) { addVideoCodec(payloadType, "VP8"); }

void Description::Video::addVP9Codec(int payloadType) { addVideoCodec(payloadType, "VP9"); }

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

		for (const auto &val : map.rtcpFbs)
			sdp << "a=rtcp-fb:" << map.pt << ' ' << val << eol;
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
			Description::Media::RTPMap map(value);
			int pt = map.pt;
			mRtpMap.emplace(pt, std::move(map));
		} else if (key == "rtcp-fb") {
			size_t p = value.find(' ');
			int pt = to_integer<int>(value.substr(0, p));
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end()) {
				PLOG_WARNING << "rtcp-fb applied before the corresponding rtpmap, ignoring";
			} else {
				it->second.rtcpFbs.emplace_back(value.substr(p + 1));
			}
		} else if (key == "fmtp") {
			size_t p = value.find(' ');
			int pt = to_integer<int>(value.substr(0, p));
			auto it = mRtpMap.find(pt);
			if (it == mRtpMap.end()) {
				PLOG_WARNING << "fmtp applied before the corresponding rtpmap, ignoring";
			} else {
				it->second.fmtps.emplace_back(value.substr(p + 1));
			}
		} else if (key == "rtcp-mux") {
			// always added
		} else {
			Entry::parseSdpLine(line);
		}
	} else if (match_prefix(line, "b=AS")) {
		mBas = to_integer<int>(line.substr(line.find(':') + 1));
	} else {
		Entry::parseSdpLine(line);
	}
}

void Description::Media::addRTPMap(const Description::Media::RTPMap& map) {
    mRtpMap.emplace(map.pt, map);
}

std::vector<uint32_t> Description::Media::getSSRCs() {
    std::vector<uint32_t> vec;
    for (auto &val : mAttributes) {
        PLOG_DEBUG << val;
        if (val.find("ssrc:") == 0) {
            vec.emplace_back(std::stoul((std::string)val.substr(5, val.find(" "))));
        }
    }
    return vec;
}


Description::Media::RTPMap::RTPMap(string_view mline) {
	size_t p = mline.find(' ');

	this->pt = to_integer<int>(mline.substr(0, p));

	string_view line = mline.substr(p + 1);
	size_t spl = line.find('/');
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
		this->encParams = line.substr(spl+1);
	}
}

void Description::Media::RTPMap::removeFB(const string &str) {
	auto it = rtcpFbs.begin();
	while (it != rtcpFbs.end()) {
		if (it->find(str) != std::string::npos) {
			it = rtcpFbs.erase(it);
		} else
			it++;
	}
}

void Description::Media::RTPMap::addFB(const string &str) { rtcpFbs.emplace_back(str); }

Description::Audio::Audio(string mid, Direction dir)
    : Media("audio 9 UDP/TLS/RTP/SAVPF", std::move(mid), dir) {}

void Description::Audio::addOpusCodec(int payloadType) {
    addAudioCodec(payloadType, "OPUS");
}

Description::Video::Video(string mid, Direction dir)
    : Media("video 9 UDP/TLS/RTP/SAVPF", std::move(mid), dir) {}

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
