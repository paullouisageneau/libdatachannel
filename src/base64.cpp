/**
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#if ENABLE_WEBSOCKET

#include "base64.hpp"

namespace rtc {

using std::to_integer;

string to_base64(const binary &data) {
	static const char tab[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

	int left = data.size() - i;
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

} // namespace rtc

#endif
