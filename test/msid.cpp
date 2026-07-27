/**
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test.hpp"

#include <rtc/description.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace rtc;
using namespace std;

namespace {

using Association = Description::Media::MediaStreamAssociation;

size_t countSubstring(const string &value, const string &needle) {
	size_t count = 0;
	size_t offset = 0;
	while ((offset = value.find(needle, offset)) != string::npos) {
		++count;
		offset += needle.size();
	}
	return count;
}

bool hasSubstring(const string &value, const string &needle) {
	return value.find(needle) != string::npos;
}

} // namespace

TestResult test_media_stream_associations() {
	try {
		const string parsedSdp =
		    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
		    "a=mid:audio\r\n"
		    "a=sendrecv\r\n"
		    "a=msid:stream1 track1\r\n"
		    "a=msid:stream2 track1\r\n"
		    "a=msid:- track1\r\n"
		    "a=msid:stream2 track1\r\n"
		    "a=msid:bad/id track1\r\n"
		    "a=msid:stream3 track1 extra\r\n"
		    "a=msid:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab track1\r\n"
		    "a=msid:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abc track1\r\n"
		    "a=x-preserved:value\r\n"
		    "a=rtpmap:111 opus/48000/2\r\n";
		Description::Media parsed(parsedSdp);
		const vector<Association> expected = {
		    {"stream1", "track1"},
		    {"stream2", "track1"},
		    {"-", "track1"},
		    {"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab", "track1"},
		};
		if (parsed.mediaStreamAssociations() != expected)
			return TestResult(false, "Valid parsed associations were not returned in SDP order");

		Description::Media withoutAppData(
		    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
		    "a=mid:audio\r\n"
		    "a=msid:stream-only\r\n"
		    "a=rtpmap:111 opus/48000/2\r\n");
		const vector<Association> expectedWithoutAppData = {{"stream-only", nullopt}};
		if (withoutAppData.mediaStreamAssociations() != expectedWithoutAppData)
			return TestResult(false, "An MSID without appdata was not parsed");

		Description::Audio generated("audio", Description::Direction::SendOnly);
		generated.addOpusCodec(111);
		generated.addAttribute("x-preserved:value");
		generated.addAttribute("ssrc:1234 msid:legacy legacy-track");
		generated.addAttribute("msid:old old-track");
		generated.setMediaStreamAssociations({
		    {"stream2", "track1"},
		    {"stream1", "track1"},
		    {"stream2", "track1"},
		});

		const vector<Association> replaced = {
		    {"stream2", "track1"},
		    {"stream1", "track1"},
		};
		if (generated.mediaStreamAssociations() != replaced)
			return TestResult(false, "Association replacement did not preserve order and deduplicate");

		const string generatedSdp = string(generated);
		if (countSubstring(generatedSdp, "a=msid:") != 2 ||
		    !hasSubstring(generatedSdp, "a=x-preserved:value") ||
		    !hasSubstring(generatedSdp, "a=ssrc:1234 msid:legacy legacy-track"))
			return TestResult(false, "Association replacement changed unrelated attributes");

		Description::Media roundTrip(generatedSdp);
		if (roundTrip.mediaStreamAssociations() != replaced)
			return TestResult(false, "Associations did not survive SDP round trip");

		const string beforeInvalidReplacement = string(generated);
		auto rejectsWithoutMutation = [&](vector<Association> associations) {
			try {
				generated.setMediaStreamAssociations(std::move(associations));
				return false;
			} catch (const invalid_argument &) {
				return string(generated) == beforeInvalidReplacement;
			}
		};

		if (!rejectsWithoutMutation({{"bad/id", "track1"}}) ||
		    !rejectsWithoutMutation({{"stream1", "bad track"}}) ||
		    !rejectsWithoutMutation({{"stream1", "track1"}, {"stream2", nullopt}}) ||
		    !rejectsWithoutMutation({{"stream1", "track1"}, {"stream2", "track2"}}))
			return TestResult(false, "Invalid association replacement was accepted or mutated state");

		generated.setMediaStreamAssociations({});
		const string clearedSdp = string(generated);
		if (!generated.mediaStreamAssociations().empty() ||
		    hasSubstring(clearedSdp, "a=msid:") ||
		    !hasSubstring(clearedSdp, "a=ssrc:1234 msid:legacy legacy-track"))
			return TestResult(false, "Clearing associations removed unrelated SSRC attributes");

		Description::Audio offer("audio", Description::Direction::SendOnly);
		offer.addOpusCodec(111);
		offer.addSSRC(1234, "audio", "remote-stream", "remote-track");
		offer.addSSRC(5678, "audio", "remote-stream", "remote-track");
		const vector<Association> offered = {{"remote-stream", "remote-track"}};
		if (offer.mediaStreamAssociations() != offered ||
		    countSubstring(string(offer), "a=msid:") != 1)
			return TestResult(false, "SSRCs for one track generated duplicate associations");

		auto answer = offer.reciprocate();
		if (!answer.mediaStreamAssociations().empty() ||
		    hasSubstring(string(answer), "a=msid:"))
			return TestResult(false, "Reciprocation copied the offerer's stream associations");

		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}
