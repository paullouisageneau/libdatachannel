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

#ifndef RTC_DESCRIPTION_H
#define RTC_DESCRIPTION_H

#include "candidate.hpp"
#include "include.hpp"

#include <iostream>
#include <map>
#include <optional>
#include <vector>

namespace rtc {

class Description {
public:
	enum class Type { Unspec = 0, Offer = 1, Answer = 2 };
	enum class Role { ActPass = 0, Passive = 1, Active = 2 };

	Description(const string &sdp, const string &typeString = "");
	Description(const string &sdp, Type type);
	Description(const string &sdp, Type type, Role role);

	Type type() const;
	string typeString() const;
	Role role() const;
	string roleString() const;
	string dataMid() const;
	std::optional<string> fingerprint() const;
	std::optional<uint16_t> sctpPort() const;
	std::optional<size_t> maxMessageSize() const;
	bool ended() const;

	void hintType(Type type);
	void setFingerprint(string fingerprint);
	void setSctpPort(uint16_t port);
	void setMaxMessageSize(size_t size);

	void addCandidate(Candidate candidate);
	void endCandidates();
	std::vector<Candidate> extractCandidates();

	bool hasMedia() const;
	void addMedia(const Description &source);

	operator string() const;
	string generateSdp(const string &eol) const;

private:
	Type mType;
	Role mRole;
	string mSessionId;
	string mIceUfrag, mIcePwd;
	std::optional<string> mFingerprint;

	// Data
	struct Data {
		string mid;
		std::optional<uint16_t> sctpPort;
		std::optional<size_t> maxMessageSize;
	};
	Data mData;

	// Media (non-data)
	struct Media {
		Media(const string &mline);
		string type;
		string description;
		string mid;
		std::vector<string> attributes;
	};
	std::map<string, Media> mMedia; // by mid

	// Candidates
	std::vector<Candidate> mCandidates;
	bool mEnded = false;

	static Type stringToType(const string &typeString);
	static string typeToString(Type type);
	static string roleToString(Role role);
};

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Description &description);

#endif
