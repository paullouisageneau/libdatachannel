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

#include "configuration.hpp"

namespace rtc {

IceServer::IceServer(const string &host) : type(Type::Stun) {
	if (size_t serviceSep = host.rfind(':'); serviceSep != string::npos) {
		if (size_t protocolSep = host.rfind(':', serviceSep - 1); protocolSep != string::npos) {
			string protocol = host.substr(0, protocolSep);
			if (protocol == "stun" || protocol == "STUN")
				type = Type::Stun;
			else if (protocol == "turn" || protocol == "TURN")
				type = Type::Turn;
			else
				throw std::invalid_argument("Unknown ICE server protocol: " + protocol);
			hostname = host.substr(protocolSep + 1, serviceSep - (protocolSep + 1));
		} else {
			hostname = host.substr(0, serviceSep);
		}
		service = host.substr(serviceSep + 1);
	} else {
		hostname = host;
		service = "3478"; // STUN UDP port
	}
}

IceServer::IceServer(string hostname_, uint16_t port_)
    : IceServer(std::move(hostname_), std::to_string(port_)) {}

IceServer::IceServer(string hostname_, string service_)
    : hostname(std::move(hostname_)), service(std::move(service_)), type(Type::Stun) {}

IceServer::IceServer(string hostname_, uint16_t port_, string username_, string password_,
                     RelayType relayType_)
    : IceServer(hostname_, std::to_string(port_), std::move(username_), std::move(password_),
                relayType_) {}

IceServer::IceServer(string hostname_, string service_, string username_, string password_,
                     RelayType relayType_)
    : hostname(std::move(hostname_)), service(std::move(service_)), type(Type::Turn),
      username(std::move(username_)), password(std::move(password_)), relayType(relayType_) {}

} // namespace rtc
