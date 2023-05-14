/**
 * Copyright (c) 2020-2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "http.hpp"

#include <algorithm>

namespace rtc::impl {

bool isHttpRequest(const byte *buffer, size_t size) {
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

size_t parseHttpLines(const byte *buffer, size_t size, std::list<string> &lines) {
	lines.clear();
	auto begin = reinterpret_cast<const char *>(buffer);
	auto end = begin + size;
	auto cur = begin;
	while (true) {
		auto last = cur;
		cur = std::find(cur, end, '\n');
		if (cur == end)
			return 0;
		string line(last, cur != begin && *std::prev(cur) == '\r' ? std::prev(cur++) : cur++);
		if (line.empty())
			break;
		lines.emplace_back(std::move(line));
	}

	return cur - begin;
}

std::multimap<string, string> parseHttpHeaders(const std::list<string> &lines) {
	std::multimap<string, string> headers;
	for (const auto &line : lines) {
		if (size_t pos = line.find_first_of(':'); pos != string::npos) {
			string key = line.substr(0, pos);
			string value = "";
			if (size_t subPos = line.find_first_not_of(' ', pos + 1); subPos != string::npos) {
				value = line.substr(subPos);
			}
			std::transform(key.begin(), key.end(), key.begin(),
			               [](char c) { return std::tolower(c); });
			headers.emplace(std::move(key), std::move(value));
		} else {
			headers.emplace(line, "");
		}
	}

	return headers;
}

} // namespace rtc::impl
