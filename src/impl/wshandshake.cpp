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

#include "wshandshake.hpp"
#include "base64.hpp"
#include "internals.hpp"
#include "sha.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <algorithm>
#include <chrono>
#include <climits>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>

using std::string;

namespace {

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

} // namespace

namespace rtc::impl {

using std::to_string;
using std::chrono::system_clock;
using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned short>;

WsHandshake::WsHandshake() {}

WsHandshake::WsHandshake(string host, string path, std::vector<string> protocols)
    : mHost(std::move(host)), mPath(std::move(path)), mProtocols(std::move(protocols)) {

	if (mHost.empty())
		throw std::invalid_argument("WebSocket HTTP host cannot be empty");

	if (mPath.empty())
		throw std::invalid_argument("WebSocket HTTP path cannot be empty");
}

string WsHandshake::host() const {
	std::unique_lock lock(mMutex);
	return mHost;
}

string WsHandshake::path() const {
	std::unique_lock lock(mMutex);
	return mPath;
}

std::vector<string> WsHandshake::protocols() const {
	std::unique_lock lock(mMutex);
	return mProtocols;
}

string WsHandshake::generateHttpRequest() {
	std::unique_lock lock(mMutex);
	mKey = generateKey();

	string out = "GET " + mPath +
	             " HTTP/1.1\r\n"
	             "Host: " +
	             mHost +
	             "\r\n"
	             "Connection: upgrade\r\n"
	             "Upgrade: websocket\r\n"
	             "Sec-WebSocket-Version: 13\r\n"
	             "Sec-WebSocket-Key: " +
	             mKey + "\r\n";

	if (!mProtocols.empty())
		out += "Sec-WebSocket-Protocol: " + implode(mProtocols, ',') + "\r\n";

	out += "\r\n";

	return out;
}

string WsHandshake::generateHttpResponse() {
	std::unique_lock lock(mMutex);
	const string out = "HTTP/1.1 101 Switching Protocols\r\n"
	                   "Server: libdatachannel\r\n"
	                   "Connection: upgrade\r\n"
	                   "Upgrade: websocket\r\n"
	                   "Sec-WebSocket-Accept: " +
	                   computeAcceptKey(mKey) + "\r\n\r\n";

	return out;
}

namespace {

string GetHttpErrorName(int responseCode) {
	switch (responseCode) {
	case 400:
		return "Bad Request";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 426:
		return "Upgrade Required";
	case 500:
		return "Internal Server Error";
	default:
		return "Error";
	}
}

} // namespace

string WsHandshake::generateHttpError(int responseCode) {
	std::unique_lock lock(mMutex);

	const string error = to_string(responseCode) + " " + GetHttpErrorName(responseCode);

	const string out = "HTTP/1.1 " + error +
	                   "\r\n"
	                   "Server: libdatachannel\r\n"
	                   "Connection: upgrade\r\n"
	                   "Upgrade: websocket\r\n"
	                   "Content-Type: text/plain\r\n"
	                   "Content-Length: " +
	                   to_string(error.size()) +
	                   "\r\n"
	                   "Access-Control-Allow-Origin: *\r\n\r\n" +
	                   error;

	return out;
}

size_t WsHandshake::parseHttpRequest(const byte *buffer, size_t size) {
	std::unique_lock lock(mMutex);
	std::list<string> lines;
	size_t length = parseHttpLines(buffer, size, lines);
	if (length == 0)
		return 0;

	if (lines.empty())
		throw RequestError("Invalid HTTP request for WebSocket", 400);

	std::istringstream requestLine(std::move(lines.front()));
	lines.pop_front();

	string method, path, protocol;
	requestLine >> method >> path >> protocol;
	PLOG_DEBUG << "WebSocket request method \"" << method << "\" for path: " << path;
	if (method != "GET")
		throw RequestError("Invalid request method \"" + method + "\" for WebSocket", 405);

	mPath = std::move(path);

	auto headers = parseHttpHeaders(lines);

	auto h = headers.find("host");
	if (h == headers.end())
		throw RequestError("WebSocket host header missing in request", 400);

	mHost = std::move(h->second);

	h = headers.find("upgrade");
	if (h == headers.end())
		throw RequestError("WebSocket upgrade header missing in request", 426);

	string upgrade;
	std::transform(h->second.begin(), h->second.end(), std::back_inserter(upgrade),
	               [](char c) { return std::tolower(c); });
	if (upgrade != "websocket")
		throw RequestError("WebSocket upgrade header mismatching: " + h->second, 426);

	h = headers.find("sec-websocket-key");
	if (h == headers.end())
		throw RequestError("WebSocket key header missing in request", 400);

	mKey = std::move(h->second);

	h = headers.find("sec-websocket-protocol");
	if (h != headers.end())
		mProtocols = explode(h->second, ',');

	return length;
}

size_t WsHandshake::parseHttpResponse(const byte *buffer, size_t size) {
	std::unique_lock lock(mMutex);
	std::list<string> lines;
	size_t length = parseHttpLines(buffer, size, lines);
	if (length == 0)
		return 0;

	if (lines.empty())
		throw Error("Invalid HTTP response for WebSocket");

	std::istringstream status(std::move(lines.front()));
	lines.pop_front();

	string protocol;
	unsigned int code = 0;
	status >> protocol >> code;
	PLOG_DEBUG << "WebSocket response code: " << code;
	if (code != 101)
		throw std::runtime_error("Unexpected response code " + to_string(code) + " for WebSocket");

	auto headers = parseHttpHeaders(lines);

	auto h = headers.find("upgrade");
	if (h == headers.end())
		throw Error("WebSocket update header missing");

	string upgrade;
	std::transform(h->second.begin(), h->second.end(), std::back_inserter(upgrade),
	               [](char c) { return std::tolower(c); });
	if (upgrade != "websocket")
		throw Error("WebSocket update header mismatching: " + h->second);

	h = headers.find("sec-websocket-accept");
	if (h == headers.end())
		throw Error("WebSocket accept header missing");

	if (h->second != computeAcceptKey(mKey))
		throw Error("WebSocket accept header is invalid");

	return length;
}

string WsHandshake::generateKey() {
	// RFC 6455: The request MUST include a header field with the name Sec-WebSocket-Key.  The value
	// of this header field MUST be a nonce consisting of a randomly selected 16-byte value that has
	// been base64-encoded. [...] The nonce MUST be selected randomly for each connection.
	auto seed = static_cast<unsigned int>(system_clock::now().time_since_epoch().count());
	random_bytes_engine generator(seed);
	binary key(16);
	auto k = reinterpret_cast<uint8_t *>(key.data());
	std::generate(k, k + key.size(), [&]() { return uint8_t(generator()); });
	return to_base64(key);
}

string WsHandshake::computeAcceptKey(const string &key) {
	return to_base64(Sha1(string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

size_t WsHandshake::parseHttpLines(const byte *buffer, size_t size, std::list<string> &lines) {
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

std::multimap<string, string> WsHandshake::parseHttpHeaders(const std::list<string> &lines) {
	std::multimap<string, string> headers;
	for (const auto &line : lines) {
		if (size_t pos = line.find_first_of(':'); pos != string::npos) {
			string key = line.substr(0, pos);
			string value = line.substr(line.find_first_not_of(' ', pos + 1));
			std::transform(key.begin(), key.end(), key.begin(),
			               [](char c) { return std::tolower(c); });
			headers.emplace(std::move(key), std::move(value));
		} else {
			headers.emplace(line, "");
		}
	}

	return headers;
}

WsHandshake::Error::Error(const string &w) : std::runtime_error(w) {}

WsHandshake::RequestError::RequestError(const string &w, int responseCode)
    : Error(w), mResponseCode(responseCode) {}

int WsHandshake::RequestError::RequestError::responseCode() const { return mResponseCode; }

} // namespace rtc::impl

#endif
