/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#ifndef RTC_ICE_CONFIGURATION_H
#define RTC_ICE_CONFIGURATION_H

#include "include.hpp"
#include "message.hpp"

#include <vector>

namespace rtc {

struct IceServer {
	enum class Type { Stun, Turn };

	// Don' Change It! It should be same order as enum NiceRelayType
	enum class RelayType { TurnUdp, TurnTcp, TurnTls };

	IceServer(const string &host_);
	IceServer(const string &hostname_, uint16_t port_);
	IceServer(const string &hostname_, const string &service_);
	IceServer(const string &hostname_, const string &service_, string username_, string password_,
	          RelayType relayType_);

	string hostname;
	string service;
	Type type;
	string username;
	string password;
	RelayType relayType;
};

struct Configuration {
	enum class LogLevel { none, fatal, error, warning, info, debug, verbose };

	Configuration(const LogLevel logLevel_ = LogLevel::error);

	std::vector<IceServer> iceServers;
	bool enableIceTcp = false;
	uint16_t portRangeBegin = 1024;
	uint16_t portRangeEnd = 65535;
};

} // namespace rtc

#endif
