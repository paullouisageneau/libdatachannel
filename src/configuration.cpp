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

using std::to_string;

IceServer::IceServer(const string &host) : type(Type::Stun) {
	if (size_t pos = host.rfind(':'); pos != string::npos) {
		hostname = host.substr(0, pos);
		service = host.substr(pos + 1);
	} else {
		hostname = host;
		service = "3478"; // STUN UDP port
	}
}

IceServer::IceServer(const string &hostname_, uint16_t port_)
    : IceServer(hostname_, to_string(port_)) {}

IceServer::IceServer(const string &hostname_, const string &service_)
    : hostname(hostname_), service(service_), type(Type::Stun) {}

IceServer::IceServer(const string &hostname_, const string &service_, string username_,
                     string password_, RelayType relayType_)
    : hostname(hostname_), service(service_), type(Type::Turn), username(username_),
      password(password_), relayType(relayType_) {}

} // namespace rtc
