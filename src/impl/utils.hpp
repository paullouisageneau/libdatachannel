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

#ifndef RTC_IMPL_UTILS_H
#define RTC_IMPL_UTILS_H

#include "common.hpp"

#include <vector>

namespace rtc::impl::utils {

std::vector<string> explode(const string &str, char delim);
string implode(const std::vector<string> &tokens, char delim);

// Decode URL percent-encoding (RFC 3986)
// See https://www.rfc-editor.org/rfc/rfc3986.html#section-2.1
string url_decode(const string &str);

// Encode as base64 (RFC 4648)
// See https://www.rfc-editor.org/rfc/rfc4648.html#section-4
string base64_encode(const binary &data);

} // namespace rtc::impl

#endif
