/**
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/global.hpp"
#include "rtc/video_layers_allocation_ext.hpp"
#include "test.hpp"

#include <cstring>
#include <cassert>

#if RTC_ENABLE_MEDIA

using namespace rtc;
using namespace std;

// Convert hex character to hex
static unsigned int char_to_hex(char ch) {
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	assert(false);
	return 0;
}

// Convert hex to binary message
static binary hex_to_binary(const char* payload) {
	binary result;

	for (size_t i = 0; i < strlen(payload); i += 2) {
		const auto hi = char_to_hex(payload[i]);
		const auto lo = char_to_hex(payload[i + 1]);

		result.push_back(static_cast<byte>((hi << 4) | lo));
	}

	return result;
}

// Convert binary message to hex
static string binary_to_hex(const binary& payload) {
	static const char* const kAlphabet = "0123456789abcdef";

	string result;
	result.reserve(payload.size() * 2);

	for (const auto b : payload) {
		const auto hi = static_cast<unsigned int>(b) >> 4;
		const auto lo = static_cast<unsigned int>(b) & 0x0F;

		result.push_back(kAlphabet[hi]);
		result.push_back(kAlphabet[lo]);
	}

	return result;
}

// The layers are null
static TestResult test_vla_null() {
	const auto payload = generateVideoLayersAllocation({}, 0);
	if (!payload.empty()) {
		return { false, "null payload should be empty" };
	}

	return { true };
}

// There are no streams
static TestResult test_vla_no_streams() {
	const auto layers = make_shared<VideoLayersAllocation>();
	const auto payload = generateVideoLayersAllocation(layers, 0);
	if (!payload.empty()) {
		return { false, "no streams should generate empty payload" };
	}

	return { true };
}

// There are streams but no spatial layers
static TestResult test_vla_no_spatial_layers() {
	const auto layers = make_shared<VideoLayersAllocation>();

	layers->rtpStreams.emplace_back();
	layers->rtpStreams.emplace_back();
	layers->rtpStreams.emplace_back();

	const auto payload = generateVideoLayersAllocation(layers, 1);
	if (!payload.empty()) {
		return { false, "no spatial layers should generate empty payload" };
	}

	return { true };
}

// There are spatial layers but no temporal layers
static TestResult test_vla_no_temporal_layers() {
	const auto layers = make_shared<VideoLayersAllocation>();

	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
	{
		VideoLayersAllocation::SpatialLayer{1280, 720, 30, {}}
	}});
	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
	{
		VideoLayersAllocation::SpatialLayer{640, 320, 30, {}}
	}});
	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
	{
		VideoLayersAllocation::SpatialLayer{320, 160, 15, {}}
	}});

	const auto payload = generateVideoLayersAllocation(layers, 1);
	if (!payload.empty()) {
		return { false, "no temporal layers should generate empty payload" };
	}

	return { true };
}

// There are two rtp streams, each with one spatial layers, each with one temporal layer
static TestResult test_vla_2_streams() {
	const auto layers = make_shared<VideoLayersAllocation>();

	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
		{
			VideoLayersAllocation::SpatialLayer{1280, 720, 30, {2500}}
	}});
	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
		{
			VideoLayersAllocation::SpatialLayer{640, 360, 30, {1500}}
	}});

	const auto payload = generateVideoLayersAllocation(layers, 0);
	if (payload.empty()) {
		return { false, "2 streams should generate a payload" };
	}

	const binary check = hex_to_binary(
		"11"	// RID = 1, NS = 2-1 = 1, sl_bm = 1
				// sl0_bm .. sl3_bm not present because sl_bm != 0
		"00"	// #tl = 4 x b00
		"C413"	// layer_0 bitrate = 2500
		"DC0B"	// layer_1 bitrate = 1500
		"04FF"	// layer_0 width-1 = 1279
		"02CF"	// layer_0 height-1 = 719
		"1E"	// layer_0 fps = 30
		"027F"	// layer_1 width-1 = 639
		"0167"	// layer_1 height-1 = 359
		"1E"	// layer_1 fps = 30
	);

	if (payload != check) {
		cout
			<< "Actual: " << binary_to_hex(payload) << "\n"
			<< "Check:  " << binary_to_hex(check) << std::endl;
		return { false, "2 streams generated invalid payload" };
	}

	return { true };
}

// There are three rtp streams, each with one spatial layers, each with one temporal layer
static TestResult test_vla_3_streams() {
	const auto layers = make_shared<VideoLayersAllocation>();

	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
		{
			VideoLayersAllocation::SpatialLayer{1280, 720, 60, {3500}}
	}});
	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
		{
			VideoLayersAllocation::SpatialLayer{640, 360, 30, {1500}}
	}});
	layers->rtpStreams.push_back(
		VideoLayersAllocation::RtpStream{
		{
			VideoLayersAllocation::SpatialLayer{320, 160, 15, {500}}
	}});

	const auto payload = generateVideoLayersAllocation(layers, 1);
	if (payload.empty()) {
		return { false, "3 streams should generate a payload" };
	}

	const binary check = hex_to_binary(
		"61"	// RID = 1, NS = 3-1 = 2, sl_bm = 1
				// sl0_bm .. sl3_bm not present because sl_bm != 0
		"00"	// #tl = 4 x b00
		"AC1B"	// layer_0 bitrate = 3500
		"DC0B"	// layer_1 bitrate = 1500
		"F403"	// layer_2 bitrate = 500
		"04FF"	// layer_0 width-1 = 1279
		"02CF"	// layer_0 height-1 = 719
		"3C"	// layer_0 fps = 60
		"027F"	// layer_1 width-1 = 639
		"0167"	// layer_1 height-1 = 359
		"1E"	// layer_1 fps = 30
		"013F"	// layer_2 width-1 = 319
		"009F"	// layer_2 height-1 = 159
		"0F"	// layer_2 fps = 15
	);

	if (payload != check) {
		cout
			<< "Actual: " << binary_to_hex(payload) << "\n"
			<< "Check:  " << binary_to_hex(check) << std::endl;
		return { false, "3 streams generated invalid payload" };
	}

	return { true };
}

TestResult test_video_layers_allocation() {
	InitLogger(LogLevel::Debug);

	if (const auto result = test_vla_null(); !result.success) {
		return result;
	}

	if (const auto result = test_vla_no_streams(); !result.success) {
		return result;
	}

	if (const auto result = test_vla_no_spatial_layers(); !result.success) {
		return result;
	}

	if (const auto result = test_vla_no_temporal_layers(); !result.success) {
		return result;
	}

	if (const auto result = test_vla_2_streams(); !result.success) {
		return result;
	}

	if (const auto result = test_vla_3_streams(); !result.success) {
		return result;
	}

	return {true};
}

#endif
