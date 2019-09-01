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
	enum class Role { Passive, Active, ActPass };

	Description(const string &sdp, Role role = Role::ActPass);

	Role role() const;
	std::optional<string> fingerprint() const;
	std::optional<uint16_t> sctpPort() const;

	void setFingerprint(string fingerprint);
	void setSctpPort(uint16_t port);
	void addCandidate(Candidate candidate);

	operator string() const;

private:
	Role mRole;
	string mSessionId;
	string mMid;
	string mIceUfrag, mIcePwd;
	std::optional<string> mFingerprint;
	std::optional<uint16_t> mSctpPort;

	std::vector<Candidate> mCandidates;
};

} // namespace rtc

std::ostream &operator<<(std::ostream &out, const rtc::Description &description);

#endif
