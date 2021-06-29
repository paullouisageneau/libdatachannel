/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
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

#ifndef RTC_IMPL_WS_HANDSHAKE_H
#define RTC_IMPL_WS_HANDSHAKE_H

#include "common.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <list>
#include <map>

namespace rtc::impl {

class WsHandshake final {
public:
	WsHandshake();
	WsHandshake(string host, string path = "/", std::vector<string> protocols = {});

	string host() const;
	string path() const;
	std::vector<string> protocols() const;

	string generateHttpRequest();
	string generateHttpResponse();
	string generateHttpError(int responseCode = 400);

	class Error : public std::runtime_error {
	public:
		explicit Error(const string &w);
	};

	class RequestError : public Error {
	public:
		explicit RequestError(const string &w, int responseCode = 400);
		int responseCode() const;

	private:
		const int mResponseCode;
	};

	size_t parseHttpRequest(const byte *buffer, size_t size);
	size_t parseHttpResponse(const byte *buffer, size_t size);

private:
	static string generateKey();
	static string computeAcceptKey(const string &key);
	static size_t parseHttpLines(const byte *buffer, size_t size, std::list<string> &lines);
	static std::multimap<string, string> parseHttpHeaders(const std::list<string> &lines);

	string mHost;
	string mPath;
	std::vector<string> mProtocols;
	string mKey;
	mutable std::mutex mMutex;
};

} // namespace rtc::impl

#endif

#endif
