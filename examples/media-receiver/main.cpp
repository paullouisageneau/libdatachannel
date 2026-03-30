/**
 * libdatachannel media receiver example
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"

#include <iostream>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int SOCKET;
#endif

#include <fstream>

using nlohmann::json;

// Write 32-bit little-endian
static void write_u32_le(std::ofstream &ofs, uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xFF);
    b[1] = static_cast<char>((v >> 8) & 0xFF);
    b[2] = static_cast<char>((v >> 16) & 0xFF);
    b[3] = static_cast<char>((v >> 24) & 0xFF);
    ofs.write(b, 4);
}

// Write 16-bit little-endian
static void write_u16_le(std::ofstream &ofs, uint16_t v) {
    char b[2];
    b[0] = static_cast<char>(v & 0xFF);
    b[1] = static_cast<char>((v >> 8) & 0xFF);
    ofs.write(b, 2);
}

// Write IVF file header (32 bytes)
static void write_ivf_file_header(std::ofstream &ofs,
                                  const char codec[4],
                                  uint16_t width,
                                  uint16_t height,
                                  uint32_t framerate_num,
                                  uint32_t framerate_den,
                                  uint32_t frame_count) {
    // Signature 'DKIF'
    ofs.write("DKIF", 4);
    // Version (2 bytes) and header size (2 bytes) -> version 0, header size 32
    write_u16_le(ofs, 0);
    write_u16_le(ofs, 32);
    // FourCC codec
    ofs.write(codec, 4);
    // Width, Height (2 bytes each)
    write_u16_le(ofs, width);
    write_u16_le(ofs, height);
    // Framerate numerator and denominator (4 bytes each)
    write_u32_le(ofs, framerate_num);
    write_u32_le(ofs, framerate_den);
    // Frame count (4 bytes)
    write_u32_le(ofs, frame_count);
    // Unused (4 bytes)
    write_u32_le(ofs, 0);
}

// Write per-frame header (12 bytes): size (4), 64-bit timestamp (we'll use 4 bytes low + 4 bytes high)
static void write_ivf_frame_header(std::ofstream &ofs, uint32_t frame_size, uint64_t timestamp) {
    write_u32_le(ofs, frame_size);
    // IVF uses a 64-bit timestamp; write low dword then high dword (little-endian)
    uint32_t ts_low = static_cast<uint32_t>(timestamp & 0xFFFFFFFFu);
    uint32_t ts_high = static_cast<uint32_t>((timestamp >> 32) & 0xFFFFFFFFu);
    write_u32_le(ofs, ts_low);
    write_u32_le(ofs, ts_high);
}

int main() {
	try {
		rtc::InitLogger(rtc::LogLevel::Debug);
		auto pc = std::make_shared<rtc::PeerConnection>();

		pc->onStateChange(
		    [](rtc::PeerConnection::State state) { std::cout << "State: " << state << std::endl; });

		pc->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
			std::cout << "Gathering State: " << state << std::endl;
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				auto description = pc->localDescription();
				json message = {{"type", description->typeString()},
				                {"sdp", std::string(description.value())}};
				std::cout << message << std::endl;
			}
		});

		rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
		media.addVP8Codec(96);
		media.setBitrate(
		    3000); // Request 3Mbps (Browsers do not encode more than 2.5MBps from a webcam)

		auto track = pc->addTrack(media);

		track->setMediaHandler(std::make_shared<rtc::VP8RtpDepacketizer>());
		track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

		std::ofstream ofs;
		ofs.open("dump.ivf", std::ios_base::out | std::ios_base::trunc);

		// Codec FourCC for VP8 is "VP80"
		const char codec[4] = { 'V','P','8','0' };

		write_ivf_file_header(ofs, codec, 1280, 720, 30, 1, 1000);

		int index = 0;
		track->onFrame([&ofs, &index](rtc::binary frame, rtc::FrameInfo info) {
			std::cout << "Got frame, size=" << frame.size() << ", timestamp=" << info.timestampSeconds->count() << std::endl;
			write_ivf_frame_header(ofs, frame.size(), index);
			ofs.write(reinterpret_cast<const char *>(frame.data()), frame.size());
			ofs.flush();
			++index;
		});

		pc->setLocalDescription();

		std::cout << "Please copy/paste the answer provided by the browser: " << std::endl;
		std::string sdp;
		std::getline(std::cin, sdp);

		std::cout << "Got answer" << sdp << std::endl;
		json j = json::parse(sdp);
		rtc::Description answer(j["sdp"].get<std::string>(), j["type"].get<std::string>());
		pc->setRemoteDescription(answer);

		std::cout << "Press any key to exit." << std::endl;
		char dummy;
		std::cin >> dummy;

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}
}
