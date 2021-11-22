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

#include "common.hpp"
#include "message.hpp"

#include <vector>

namespace rtc {

struct RTC_CPP_EXPORT IceServer {
	enum class Type { Stun, Turn };
	enum class RelayType { TurnUdp, TurnTcp, TurnTls };

	// Any type
	IceServer(const string &url);

	// STUN
	IceServer(string hostname_, uint16_t port_);
	IceServer(string hostname_, string service_);

	// TURN
	IceServer(string hostname_, uint16_t port, string username_, string password_,
	          RelayType relayType_ = RelayType::TurnUdp);
	IceServer(string hostname_, string service_, string username_, string password_,
	          RelayType relayType_ = RelayType::TurnUdp);

	string hostname;
	uint16_t port;
	Type type;
	string username;
	string password;
	RelayType relayType;
};

struct RTC_CPP_EXPORT ProxyServer {
	enum class Type { None = 0, Socks5, Http, Last = Http };

	ProxyServer(Type type_, string hostname_, uint16_t port_, string username_ = "",
	            string password_ = "");

	Type type;
	string hostname;
	uint16_t port;
	string username;
	string password;
};

enum class CertificateType {
	Default = RTC_CERTIFICATE_DEFAULT, // ECDSA
	Ecdsa = RTC_CERTIFICATE_ECDSA,
	Rsa = RTC_CERTIFICATE_RSA
};

enum class TransportPolicy { All = RTC_TRANSPORT_POLICY_ALL, Relay = RTC_TRANSPORT_POLICY_RELAY };

struct RTC_CPP_EXPORT Configuration {
	// ICE settings
	std::vector<IceServer> iceServers;
	optional<ProxyServer> proxyServer; // libnice only
	optional<string> bindAddress;      // libjuice only, default any

	// Options
	CertificateType certificateType = CertificateType::Default;
	TransportPolicy iceTransportPolicy = TransportPolicy::All;
	bool enableIceTcp = false;
	bool disableAutoNegotiation = false;

	// Port range
	uint16_t portRangeBegin = 1024;
	uint16_t portRangeEnd = 65535;

	// Network MTU
	optional<size_t> mtu;

	// Local maximum message size for Data Channels
	optional<size_t> maxMessageSize;
};

} // namespace rtc

#endif
