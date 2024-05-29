/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_ICE_CONFIGURATION_H
#define RTC_ICE_CONFIGURATION_H

#include "common.hpp"

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
	enum class Type { Http, Socks5 };

	ProxyServer(const string &url);

	ProxyServer(Type type_, string hostname_, uint16_t port_);
	ProxyServer(Type type_, string hostname_, uint16_t port_, string username_, string password_);

	Type type;
	string hostname;
	uint16_t port;
	optional<string> username;
	optional<string> password;
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
	bool enableIceTcp = false;    // libnice only
	bool enableIceUdpMux = false; // libjuice only
	bool disableAutoNegotiation = false;
	bool disableAutoGathering = false;
	bool forceMediaTransport = false;
	bool disableFingerprintVerification = false;

	// Port range
	uint16_t portRangeBegin = 1024;
	uint16_t portRangeEnd = 65535;

	// Network MTU
	optional<size_t> mtu;

	// Local maximum message size for Data Channels
	optional<size_t> maxMessageSize;

	// Certificates and private keys
	optional<string> certificatePemFile;
	optional<string> keyPemFile;
	optional<string> keyPemPass;
};

#ifdef RTC_ENABLE_WEBSOCKET

struct WebSocketConfiguration {
	bool disableTlsVerification = false; // if true, don't verify the TLS certificate
	optional<ProxyServer> proxyServer;   // only non-authenticated http supported for now
	std::vector<string> protocols;
	optional<std::chrono::milliseconds> connectionTimeout; // zero to disable
	optional<std::chrono::milliseconds> pingInterval;      // zero to disable
	optional<int> maxOutstandingPings;
	optional<string> caCertificatePemFile;
	optional<string> certificatePemFile;
	optional<string> keyPemFile;
	optional<string> keyPemPass;
	optional<size_t> maxMessageSize;
};

struct WebSocketServerConfiguration {
	uint16_t port = 8080;
	bool enableTls = false;
	optional<string> certificatePemFile;
	optional<string> keyPemFile;
	optional<string> keyPemPass;
	optional<string> bindAddress;
	optional<std::chrono::milliseconds> connectionTimeout;
	optional<size_t> maxMessageSize;
};

#endif

} // namespace rtc

#endif
