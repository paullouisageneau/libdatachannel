/**
 * Copyright (c) 2026 Nemirtingas
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

namespace {

// Test CA, self-signed, valid from 2025-01-01 to 2125-01-01
const string TestCaCertificatePem = R"(-----BEGIN CERTIFICATE-----
MIIBdTCCARugAwIBAgIBATAKBggqhkjOPQQDAjAhMR8wHQYDVQQDDBZsaWJkYXRh
Y2hhbm5lbCB0ZXN0IENBMCAXDTI1MDEwMTAwMDAwMFoYDzIxMjUwMTAxMDAwMDAw
WjAhMR8wHQYDVQQDDBZsaWJkYXRhY2hhbm5lbCB0ZXN0IENBMFkwEwYHKoZIzj0C
AQYIKoZIzj0DAQcDQgAEvS0uAt9gD9DrolbPcTg637cj65z1B4lW8b9V2swddFkt
T01SAKi6h0ld8Q5oVoVjl5wvcyIYrKfmOi7PO5NWGqNCMEAwDwYDVR0TAQH/BAUw
AwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFLdEDrYqRGMJg1yfqyEQVAWM
gpyQMAoGCCqGSM49BAMCA0gAMEUCIFQrthe8HMT3o457WzC+HINzDZ0RalZaSDC8
oEuCvMZCAiEArB8IE//OT5Hvz2A0JWPvcht0/ttMKxeQUUpr+mYZ3Lw=
-----END CERTIFICATE-----
)";

// Server certificate signed by the test CA, for "localhost" only (no IP address)
const string TestServerCertificatePem = R"(-----BEGIN CERTIFICATE-----
MIIBtDCCAVmgAwIBAgIBAjAKBggqhkjOPQQDAjAhMR8wHQYDVQQDDBZsaWJkYXRh
Y2hhbm5lbCB0ZXN0IENBMCAXDTI1MDEwMTAwMDAwMFoYDzIxMjUwMTAxMDAwMDAw
WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNC
AAQWwVUDRK0SgDVKX6PTWluSUGpj40wlnEQV87AC/EtRgIyyOrByT8KtnC5iaLef
eubeDh2vpqbtFBeA+x/U3epJo4GMMIGJMAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/
BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBQGA1UdEQQNMAuCCWxvY2FsaG9z
dDAdBgNVHQ4EFgQU9JiU5aAP5tQvkDp7w6GuPqHYvhAwHwYDVR0jBBgwFoAUt0QO
tipEYwmDXJ+rIRBUBYyCnJAwCgYIKoZIzj0EAwIDSQAwRgIhAIjM3prGJyhT5veT
U/jiLc9qvAd5gblOL/qgS/mtBusJAiEAvRRpRZwc7mweyZsb4wfb3+RC3uMqUhyw
T8cGTxtgjtI=
-----END CERTIFICATE-----
)";

const string TestServerKeyPem = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgVVuTGNUYeLkHB+Vl
Aqu5lLP3sAF+Q8ugRj8AkW0sJpyhRANCAAQWwVUDRK0SgDVKX6PTWluSUGpj40wl
nEQV87AC/EtRgIyyOrByT8KtnC5iaLefeubeDh2vpqbtFBeA+x/U3epJ
-----END PRIVATE KEY-----
)";

const uint16_t TestPort = 48090;

// Connect a client and return true if the WebSocket opens, false if it fails or times out
bool try_connect(const string &url, WebSocket::Configuration config) {
	auto ws = make_shared<WebSocket>(std::move(config));

	auto result = make_shared<promise<bool>>();
	auto done = make_shared<atomic<bool>>(false);
	auto settle = [result, done](bool open) {
		if (!done->exchange(true))
			result->set_value(open);
	};

	ws->onOpen([settle]() { settle(true); });
	ws->onError([settle](string error) {
		cout << "WebSocket: Error: " << error << endl;
		settle(false);
	});
	ws->onClosed([settle]() { settle(false); });

	ws->open(url);

	auto future = result->get_future();
	bool open = false;
	if (future.wait_for(10s) == future_status::ready)
		open = future.get();
	else
		cout << "WebSocket: Timeout" << endl;

	ws->resetCallbacks();
	ws->close();

	int attempts = 50;
	while (!ws->isClosed() && attempts--)
		this_thread::sleep_for(100ms);

	return open;
}

} // namespace

TestResult test_websocket_tls_verification() {
	InitLogger(LogLevel::Debug);

	WebSocketServer::Configuration serverConfig;
	serverConfig.port = TestPort;
	serverConfig.enableTls = true;
	serverConfig.certificatePemFile = TestServerCertificatePem;
	serverConfig.keyPemFile = TestServerKeyPem;
	serverConfig.bindAddress = "127.0.0.1";
	WebSocketServer server(std::move(serverConfig));

	std::mutex clientsMutex;
	std::vector<shared_ptr<WebSocket>> clients;
	server.onClient([&clientsMutex, &clients](shared_ptr<WebSocket> incoming) {
		std::lock_guard lock(clientsMutex);
		clients.push_back(std::move(incoming));
	});

	const string url = "wss://localhost:" + to_string(TestPort) + "/";
	const string ipUrl = "wss://127.0.0.1:" + to_string(TestPort) + "/";

	TestResult result(true);
	auto expect = [&result](const string &name, bool expectedOpen, bool open) {
		cout << "WebSocket TLS verification: " << name << ": " << (open ? "accepted" : "rejected")
		     << endl;
		if (open != expectedOpen && result.success)
			result = TestResult(false, name + ": connection was " +
			                               (open ? "accepted" : "rejected") + " unexpectedly");
	};

	{
		// The test CA is not trusted by the system
		WebSocket::Configuration config;
		expect("untrusted CA", false, try_connect(url, std::move(config)));
	}
	{
		WebSocket::Configuration config;
		config.caCertificatePemFile = TestCaCertificatePem;
		expect("custom CA", true, try_connect(url, std::move(config)));
	}
	{
		// The certificate is valid for "localhost" but not for 127.0.0.1
		WebSocket::Configuration config;
		config.caCertificatePemFile = TestCaCertificatePem;
		expect("custom CA with host mismatch", false, try_connect(ipUrl, std::move(config)));
	}
	{
		WebSocket::Configuration config;
		config.disableTlsVerification = true;
		expect("verification disabled", true, try_connect(url, std::move(config)));
	}

	server.stop();

	{
		std::lock_guard lock(clientsMutex);
		clients.clear();
	}
	this_thread::sleep_for(1s);

	return result;
}

#endif
