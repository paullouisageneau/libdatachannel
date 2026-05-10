/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "rtc/rtp.hpp"
#include "test.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace rtc;
using namespace std;

// Unit test: parse a single RTCP APP packet through the AppHandler
TestResult test_rtcp_app_single_packet() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP APP single packet test" << endl;

	uint8_t receivedSubtype = 0;
	RtcpAppName receivedName;
	binary receivedData;
	int callbackCount = 0;

	auto handler = make_shared<AppHandler>(
	    [&](RtcpAppName name, uint8_t subtype, binary data) {
		    receivedSubtype = subtype;
		    receivedName = std::move(name);
		    receivedData = std::move(data);
		    callbackCount++;
	    });

	// Build an RTCP APP packet: subtype=5, name="TEST", 8 bytes of data
	const uint8_t subtype = 5;
	RtcpAppName name = {'T', 'E', 'S', 'T'};
	const binary appData = {byte{0x01}, byte{0x02}, byte{0x03}, byte{0x04},
	                         byte{0x05}, byte{0x06}, byte{0x07}, byte{0x08}};
	const SSRC ssrc = 12345;

	size_t packetSize = RtcpApp::SizeWithData(appData.size());
	auto message = make_message(packetSize, Message::Control);
	auto *app = reinterpret_cast<RtcpApp *>(message->data());
	app->preparePacket(ssrc, name, subtype, appData.size());
	std::memcpy(app->_data, appData.data(), appData.size());

	// Verify struct accessors
	if (app->ssrc() != ssrc)
		return TestResult(false, "RtcpApp::ssrc() mismatch");
	if (app->subtype() != subtype)
		return TestResult(false, "RtcpApp::subtype() mismatch");
	if (app->name() != name) {
		RtcpAppName parsedName = app->name();
		return TestResult(false, "RtcpApp::name() mismatch: got " + std::string(parsedName.data(), parsedName.size()));
	}
	if (app->dataSize() != appData.size())
		return TestResult(false, "RtcpApp::dataSize() mismatch: expected " +
		                             to_string(appData.size()) + " got " +
		                             to_string(app->dataSize()));

	// Feed to handler
	message_vector messages;
	messages.push_back(std::move(message));
	handler->incoming(messages, [](message_ptr) {});

	if (callbackCount != 1)
		return TestResult(false, "Expected 1 callback, got " + to_string(callbackCount));
	if (receivedSubtype != subtype)
		return TestResult(false, "Subtype mismatch in callback");
	if (receivedName != name)
		return TestResult(false, "Name mismatch in callback: got " + std::string(receivedName.data(), receivedName.size()));
	if (receivedData.size() != appData.size())
		return TestResult(false, "Data size mismatch in callback");
	if (std::memcmp(receivedData.data(), appData.data(), appData.size()) != 0)
		return TestResult(false, "Data content mismatch in callback");

	cout << "RTCP APP single packet test passed" << endl;
	return TestResult(true);
}

// Unit test: parse RTCP APP packet from a compound RTCP packet (SR + APP)
TestResult test_rtcp_app_compound_packet() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP APP compound packet test" << endl;

	uint8_t receivedSubtype = 0;
	RtcpAppName receivedName;
	binary receivedData;
	int callbackCount = 0;

	auto handler = make_shared<AppHandler>(
	    [&](RtcpAppName name, uint8_t subtype, binary data) {
		    receivedSubtype = subtype;
		    receivedName = std::move(name);
		    receivedData = std::move(data);
		    callbackCount++;
	    });

	// Build a compound RTCP packet: Sender Report (PT=200) followed by APP (PT=204)
	const uint8_t subtype = 3;
	RtcpAppName name = {'A', 'B', 'C', 'D'};
	const binary appData = {byte{0xAA}, byte{0xBB}, byte{0xCC}, byte{0xDD}};
	const SSRC ssrc = 99999;

	// Build a minimal SR (no report blocks)
	size_t srSize = RtcpSr::Size(0);
	size_t appSize = RtcpApp::SizeWithData(appData.size());
	size_t totalSize = srSize + appSize;

	auto message = make_message(totalSize, Message::Control);

	// Fill in the SR portion
	auto *sr = reinterpret_cast<RtcpSr *>(message->data());
	sr->preparePacket(55555, 0);
	sr->setNtpTimestamp(0);
	sr->setRtpTimestamp(0);
	sr->setPacketCount(10);
	sr->setOctetCount(1000);

	// Fill in the APP portion after the SR
	auto *app = reinterpret_cast<RtcpApp *>(message->data() + srSize);
	app->preparePacket(ssrc, name, subtype, appData.size());
	std::memcpy(app->_data, appData.data(), appData.size());

	// Feed compound packet to handler
	message_vector messages;
	messages.push_back(std::move(message));
	handler->incoming(messages, [](message_ptr) {});

	if (callbackCount != 1)
		return TestResult(false, "Expected 1 callback from compound packet, got " +
		                             to_string(callbackCount));
	if (receivedSubtype != subtype)
		return TestResult(false, "Subtype mismatch in compound packet");
	if (receivedName != name)
		return TestResult(false, "Name mismatch in compound packet: got " + std::string(receivedName.data(), receivedName.size()));
	if (receivedData.size() != appData.size())
		return TestResult(false, "Data size mismatch in compound packet");
	if (std::memcmp(receivedData.data(), appData.data(), appData.size()) != 0)
		return TestResult(false, "Data content mismatch in compound packet");

	cout << "RTCP APP compound packet test passed" << endl;
	return TestResult(true);
}

// Unit test: APP packet with no application-dependent data (name only)
TestResult test_rtcp_app_empty_data() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP APP empty data test" << endl;

	uint8_t receivedSubtype = 0;
	RtcpAppName receivedName;
	binary receivedData;
	int callbackCount = 0;

	auto handler = make_shared<AppHandler>(
	    [&](RtcpAppName name, uint8_t subtype, binary data) {
		    receivedSubtype = subtype;
		    receivedName = std::move(name);
		    receivedData = std::move(data);
		    callbackCount++;
	    });

	const uint8_t subtype = 0;
	RtcpAppName name = {'P', 'I', 'N', 'G'};
	const SSRC ssrc = 42;

	size_t packetSize = RtcpApp::SizeWithData(0);
	auto message = make_message(packetSize, Message::Control);
	auto *app = reinterpret_cast<RtcpApp *>(message->data());
	app->preparePacket(ssrc, name, subtype, 0);

	if (app->dataSize() != 0)
		return TestResult(false, "Expected dataSize=0 for empty APP packet");

	message_vector messages;
	messages.push_back(std::move(message));
	handler->incoming(messages, [](message_ptr) {});

	if (callbackCount != 1)
		return TestResult(false, "Expected 1 callback for empty APP, got " + to_string(callbackCount));
	if (receivedSubtype != 0)
		return TestResult(false, "Subtype mismatch for empty APP");
	if (receivedName != name)
		return TestResult(false, "Name mismatch for empty APP: got " + std::string(receivedName.data(), receivedName.size()));
	if (!receivedData.empty())
		return TestResult(false, "Expected empty data for empty APP");

	cout << "RTCP APP empty data test passed" << endl;
	return TestResult(true);
}

// Unit test: sendApp constructs a valid packet that can be parsed back
TestResult test_rtcp_app_send() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP APP send test" << endl;

	auto handler = make_shared<AppHandler>([](RtcpAppName, uint8_t, binary) {});

	const SSRC ssrc = 77777;
	const uint8_t subtype = 15;
	RtcpAppName name = {'S', 'E', 'N', 'D'};
	const binary appData = {byte{0x10}, byte{0x20}, byte{0x30}, byte{0x40},
	                         byte{0x50}, byte{0x60}, byte{0x70}, byte{0x80},
	                         byte{0x90}, byte{0xA0}, byte{0xB0}, byte{0xC0}};

	message_ptr sentMessage;
	message_callback captureSend = [&](message_ptr msg) { sentMessage = std::move(msg); };

	bool result = handler->sendRtcpApp(ssrc, name, subtype, appData, captureSend);
	if (!result)
		return TestResult(false, "sendApp returned false");
	if (!sentMessage)
		return TestResult(false, "No message was sent");

	// Parse the sent message and verify its contents
	auto *app = reinterpret_cast<RtcpApp *>(sentMessage->data());
	if (app->header.payloadType() != 204)
		return TestResult(false, "Sent packet has wrong payload type: " +
		                             to_string(app->header.payloadType()));
	if (app->ssrc() != ssrc)
		return TestResult(false, "Sent packet has wrong SSRC");
	if (app->subtype() != subtype)
		return TestResult(false, "Sent packet has wrong subtype");
	RtcpAppName sentName = app->name();
	if (sentName != name)
		return TestResult(false, "Sent packet has wrong name: " + std::string(sentName.data(), sentName.size()));
	if (app->dataSize() != appData.size())
		return TestResult(false, "Sent packet has wrong data size");
	if (std::memcmp(app->_data, appData.data(), appData.size()) != 0)
		return TestResult(false, "Sent packet has wrong data content");

	// Verify that the sent packet can be re-parsed through incoming
	uint8_t receivedSubtype = 0;
	RtcpAppName receivedName;
	binary receivedData;
	int callbackCount = 0;

	auto handler2 = make_shared<AppHandler>(
	    [&](RtcpAppName n, uint8_t st, binary d) {
		    receivedSubtype = st;
		    receivedName = std::move(n);
		    receivedData = std::move(d);
		    callbackCount++;
	    });

	message_vector messages;
	messages.push_back(std::move(sentMessage));
	handler2->incoming(messages, [](message_ptr) {});

	if (callbackCount != 1)
		return TestResult(false, "Round-trip: expected 1 callback, got " + to_string(callbackCount));
	if (receivedSubtype != subtype)
		return TestResult(false, "Round-trip: subtype mismatch");
	if (receivedName != name)
		return TestResult(false, "Round-trip: name mismatch");
	if (receivedData.size() != appData.size())
		return TestResult(false, "Round-trip: data size mismatch");
	if (std::memcmp(receivedData.data(), appData.data(), appData.size()) != 0)
		return TestResult(false, "Round-trip: data content mismatch");

	cout << "RTCP APP send test passed" << endl;
	return TestResult(true);
}

// Unit test: multiple APP packets in a single compound message
TestResult test_rtcp_app_multiple_in_compound() {
	InitLogger(LogLevel::Debug);
	cout << "RTCP APP multiple in compound test" << endl;

	vector<tuple<RtcpAppName, uint8_t, binary>> received;

	auto handler = make_shared<AppHandler>(
	    [&](RtcpAppName name, uint8_t subtype, binary data) {
		    received.emplace_back(std::move(name), subtype, std::move(data));
	    });

	// Build a compound message with two APP packets back-to-back
	RtcpAppName name1 = {'O', 'N', 'E', '!'};
	const binary data1 = {byte{0x11}, byte{0x22}, byte{0x33}, byte{0x44}};
	RtcpAppName name2 = {'T', 'W', 'O', '!'};
	const binary data2 = {byte{0xAA}, byte{0xBB}, byte{0xCC}, byte{0xDD},
	                       byte{0xEE}, byte{0xFF}, byte{0x00}, byte{0x11}};

	size_t size1 = RtcpApp::SizeWithData(data1.size());
	size_t size2 = RtcpApp::SizeWithData(data2.size());
	size_t totalSize = size1 + size2;

	auto message = make_message(totalSize, Message::Control);

	auto *app1 = reinterpret_cast<RtcpApp *>(message->data());
	app1->preparePacket(1000, name1, 1, data1.size());
	std::memcpy(app1->_data, data1.data(), data1.size());

	auto *app2 = reinterpret_cast<RtcpApp *>(message->data() + size1);
	app2->preparePacket(2000, name2, 31, data2.size());
	std::memcpy(app2->_data, data2.data(), data2.size());

	message_vector messages;
	messages.push_back(std::move(message));
	handler->incoming(messages, [](message_ptr) {});

	if (received.size() != 2)
		return TestResult(false, "Expected 2 callbacks, got " + to_string(received.size()));

	// Verify first APP
	auto &[n1, st1, d1] = received[0];
	if (st1 != 1)
		return TestResult(false, "First APP subtype mismatch");
	if (n1 != name1)
		return TestResult(false, "First APP name mismatch: got " + std::string(n1.data(), n1.size()));
	if (d1.size() != data1.size() || std::memcmp(d1.data(), data1.data(), data1.size()) != 0)
		return TestResult(false, "First APP data mismatch");

	// Verify second APP
	auto &[n2, st2, d2] = received[1];
	if (st2 != 31)
		return TestResult(false, "Second APP subtype mismatch");
	if (n2 != name2)
		return TestResult(false, "Second APP name mismatch: got " + std::string(n2.data(), n2.size()));
	if (d2.size() != data2.size() || std::memcmp(d2.data(), data2.data(), data2.size()) != 0)
		return TestResult(false, "Second APP data mismatch");

	cout << "RTCP APP multiple in compound test passed" << endl;
	return TestResult(true);
}
