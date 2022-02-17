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

#include <iterator>
#include <sstream>

namespace rtc::impl::utils {

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

} // namespace rtc::impl::utils
