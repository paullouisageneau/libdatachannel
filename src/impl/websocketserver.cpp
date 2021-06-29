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

#if RTC_ENABLE_WEBSOCKET

#include "websocketserver.hpp"
#include "common.hpp"
#include "internals.hpp"
#include "threadpool.hpp"

namespace rtc::impl {

using namespace std::placeholders;

WebSocketServer::WebSocketServer(Configuration config_)
    : config(std::move(config_)), tcpServer(std::make_unique<TcpServer>(config.port)),
      mStopped(false) {
	PLOG_VERBOSE << "Creating WebSocketServer";

	if (config.enableTls) {
		if (config.certificatePemFile && config.keyPemFile) {
			mCertificate = std::make_shared<Certificate>(Certificate::FromFile(
			    *config.certificatePemFile, *config.keyPemFile, config.keyPemPass.value_or("")));

		} else if (!config.certificatePemFile && !config.keyPemFile) {
			mCertificate = std::make_shared<Certificate>(
			    Certificate::Generate(CertificateType::Default, "localhost"));
		} else {
			throw std::invalid_argument(
			    "Either none or both certificate and key PEM files must be specified");
		}
	}

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
