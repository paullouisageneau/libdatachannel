/**
 * Copyright (c) 2020-2022 Paul-Louis Ageneau
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

#include "utils.hpp"

#include "impl/internals.hpp"

#include <cctype>
#include <functional>
#include <iterator>
#include <sstream>

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

} // namespace rtc::impl::utils
