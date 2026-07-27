/**
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/description.hpp"
#include "test.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rtc;
using namespace std;

namespace {

vector<string> codecAttributeLines(const string &sdp) {
	vector<string> lines;
	string line;
	istringstream stream(sdp);
	while (getline(stream, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.rfind("a=rtpmap:", 0) == 0 || line.rfind("a=fmtp:", 0) == 0 ||
		    (line.rfind("a=rtcp-fb:", 0) == 0 && line.rfind("a=rtcp-fb:*", 0) != 0))
			lines.push_back(std::move(line));
	}
	return lines;
}

vector<int> rtpMapPayloadTypes(const string &sdp) {
	vector<int> payloadTypes;
	for (const auto &line : codecAttributeLines(sdp)) {
		constexpr string_view prefix = "a=rtpmap:";
		if (line.rfind(prefix, 0) != 0)
			continue;

		const auto end = line.find(' ', prefix.size());
		payloadTypes.push_back(stoi(line.substr(prefix.size(), end - prefix.size())));
	}
	return payloadTypes;
}

} // namespace

TestResult test_description_codec_order() {
	const string mediaSdp =
	    "m=video 9 UDP/TLS/RTP/SAVPF 102 121 96 97\r\n"
	    "c=IN IP4 0.0.0.0\r\n"
	    "a=mid:video\r\n"
	    "a=sendrecv\r\n"
	    "a=rtcp-mux\r\n"
	    "a=rtpmap:96 VP8/90000\r\n"
	    "a=rtcp-fb:96 nack\r\n"
	    "a=rtpmap:102 H264/90000\r\n"
	    "a=fmtp:102 profile-level-id=42e01f\r\n"
	    "a=rtpmap:97 rtx/90000\r\n"
	    "a=fmtp:97 apt=96\r\n"
	    "a=rtpmap:121 rtx/90000\r\n"
	    "a=fmtp:121 apt=102\r\n";
	const vector<string> expectedLines = {
	    "a=rtpmap:102 H264/90000",
	    "a=fmtp:102 profile-level-id=42e01f",
	    "a=rtpmap:121 rtx/90000",
	    "a=fmtp:121 apt=102",
	    "a=rtpmap:96 VP8/90000",
	    "a=rtcp-fb:96 nack",
	    "a=rtpmap:97 rtx/90000",
	    "a=fmtp:97 apt=96",
	};

	Description::Media parsed(mediaSdp);
	if (codecAttributeLines(parsed.generateSdp()) != expectedLines)
		return TestResult(false, "Parsed codec attributes do not follow m-line order");

	Description::Video constructed("video", Description::Direction::SendOnly);
	constructed.addH264Codec(102);
	constructed.addVP8Codec(96);
	if (rtpMapPayloadTypes(constructed.generateSdp()) != vector<int>{102, 96})
		return TestResult(false, "Added codec attributes do not follow insertion order");

	constructed.removeRtpMap(102);
	constructed.addH264Codec(102);
	if (rtpMapPayloadTypes(constructed.generateSdp()) != vector<int>{96, 102})
		return TestResult(false, "Re-added codec attributes do not move to the end");

	return TestResult(true);
}
