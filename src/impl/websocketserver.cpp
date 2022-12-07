/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_WEBSOCKET

#include "websocketserver.hpp"
#include "common.hpp"
#include "internals.hpp"
#include "threadpool.hpp"

namespace rtc::impl {

using namespace std::placeholders;

const string PemBeginCertificateTag = "-----BEGIN CERTIFICATE-----";

WebSocketServer::WebSocketServer(Configuration config_)
    : config(std::move(config_)), mStopped(false) {
	PLOG_VERBOSE << "Creating WebSocketServer";

	// Create certificate
	if (config.enableTls) {
		if (config.certificatePemFile && config.keyPemFile) {
			mCertificate = std::make_shared<Certificate>(
			    config.certificatePemFile->find(PemBeginCertificateTag) != string::npos
			        ? Certificate::FromString(*config.certificatePemFile, *config.keyPemFile)
			        : Certificate::FromFile(*config.certificatePemFile, *config.keyPemFile,
			                                config.keyPemPass.value_or("")));

		} else if (!config.certificatePemFile && !config.keyPemFile) {
			mCertificate = std::make_shared<Certificate>(
			    Certificate::Generate(CertificateType::Default, "localhost"));
		} else {
			throw std::invalid_argument(
			    "Either none or both certificate and key PEM files must be specified");
		}
	}

	// Create TCP server
	tcpServer = std::make_unique<TcpServer>(config.port);

	// Create server thread
	mThread = std::thread(&WebSocketServer::runLoop, this);
}

WebSocketServer::~WebSocketServer() {
	PLOG_VERBOSE << "Destroying WebSocketServer";
	stop();
}

void WebSocketServer::stop() {
	if (mStopped.exchange(true))
		return;

	PLOG_DEBUG << "Stopping WebSocketServer thread";
	tcpServer->close();
	mThread.join();
}

void WebSocketServer::runLoop() {
	PLOG_INFO << "Starting WebSocketServer";

	try {
		while (auto incoming = tcpServer->accept()) {
			try {
				if (!clientCallback)
					continue;

				auto impl = std::make_shared<WebSocket>(nullopt, mCertificate);
				impl->changeState(WebSocket::State::Connecting);
				impl->setTcpTransport(incoming);
				clientCallback(std::make_shared<rtc::WebSocket>(impl));

			} catch (const std::exception &e) {
				PLOG_ERROR << "WebSocketServer: " << e.what();
			}
		}
	} catch (const std::exception &e) {
		PLOG_FATAL << "WebSocketServer: " << e.what();
	}

	PLOG_INFO << "Stopped WebSocketServer";
}

} // namespace rtc::impl

#endif
