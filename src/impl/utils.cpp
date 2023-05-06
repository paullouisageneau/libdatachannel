/**
 * Copyright (c) 2020-2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "utils.hpp"

#include "impl/internals.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <iterator>
#include <sstream>
#include <thread>

namespace rtc::impl::utils {

using std::to_integer;

std::vector<string> explode(const string &str, char delim) {
	std::vector<std::string> result;
	std::istringstream ss(str);
	string token;
	while (std::getline(ss, token, delim))
		result.push_back(token);

	return result;
}

string implode(const std::vector<string> &tokens, char delim) {
	string sdelim(1, delim);
	std::ostringstream ss;
	std::copy(tokens.begin(), tokens.end(), std::ostream_iterator<string>(ss, sdelim.c_str()));
	string result = ss.str();
	if (result.size() > 0)
		result.resize(result.size() - 1);

	return result;
}

string url_decode(const string &str) {
	string result;
	size_t i = 0;
	while (i < str.size()) {
		char c = str[i++];
		if (c == '%') {
			auto value = str.substr(i, 2);
			try {
				if (value.size() != 2 || !std::isxdigit(value[0]) || !std::isxdigit(value[1]))
					throw std::exception();

				c = static_cast<char>(std::stoi(value, nullptr, 16));
				i += 2;

			} catch (...) {
				PLOG_WARNING << "Invalid percent-encoded character in URL: \"%" + value + "\"";
			}
		}

		result.push_back(c);
	}

	return result;
}

string base64_encode(const binary &data) {
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	string out;
	out.reserve(3 * ((data.size() + 3) / 4));
	int i = 0;
	while (data.size() - i >= 3) {
		auto d0 = to_integer<uint8_t>(data[i]);
		auto d1 = to_integer<uint8_t>(data[i + 1]);
		auto d2 = to_integer<uint8_t>(data[i + 2]);
		out += tab[d0 >> 2];
		out += tab[((d0 & 3) << 4) | (d1 >> 4)];
		out += tab[((d1 & 0x0F) << 2) | (d2 >> 6)];
		out += tab[d2 & 0x3F];
		i += 3;
	}

	int left = int(data.size() - i);
	if (left) {
		auto d0 = to_integer<uint8_t>(data[i]);
		out += tab[d0 >> 2];
		if (left == 1) {
			out += tab[(d0 & 3) << 4];
			out += '=';
		} else { // left == 2
			auto d1 = to_integer<uint8_t>(data[i + 1]);
			out += tab[((d0 & 3) << 4) | (d1 >> 4)];
			out += tab[(d1 & 0x0F) << 2];
		}
		out += '=';
	}

	return out;
}

std::seed_seq random_seed() {
	std::vector<unsigned int> seed;

	// Seed with random device
	try {
		// On some systems an exception might be thrown if the random_device can't be initialized
		std::random_device device;
		// 128 bits should be more than enough
		std::generate_n(std::back_inserter(seed), 4, std::ref(device));
	} catch (...) {
		// Ignore
	}

	// Seed with high-resolution clock
	using std::chrono::high_resolution_clock;
	seed.push_back(
	    static_cast<unsigned int>(high_resolution_clock::now().time_since_epoch().count()));

	// Seed with thread id
	seed.push_back(
	    static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));

	return std::seed_seq(seed.begin(), seed.end());
}

bool IsHttpRequest(const byte *buffer, size_t size) {
	// Check the buffer starts with a valid-looking HTTP method
	for (size_t i = 0; i < size; ++i) {
		char c = static_cast<char>(buffer[i]);
		if (i > 0 && c == ' ')
			break;
		else if (i >= 8 || c < 'A' || c > 'Z')
			return false;
	}
	return true;
}

} // namespace rtc::impl::utils
