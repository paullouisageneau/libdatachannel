/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <rtc/rtc.h>

#if RTC_ENABLE_WEBSOCKET

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
static void sleep(unsigned int secs) { Sleep(secs * 1000); }
#else
#include <unistd.h> // for sleep
#endif

static const char *MESSAGE = "Hello, this is a C API WebSocket test!";

static bool success = false;
static bool failed = false;
static int wsclient = -1;

static void RTC_API openCallback(int ws, void *ptr) {
	printf("WebSocket: Connection open\n");

	if (rtcSendMessage(ws, MESSAGE, -1) < 0) { // negative size indicates a null-terminated string
		fprintf(stderr, "rtcSendMessage failed\n");
		failed = true;
		return;
	}
}

static void RTC_API closedCallback(int ws, void *ptr) { printf("WebSocket: Connection closed"); }

static void RTC_API messageCallback(int ws, const char *message, int size, void *ptr) {
	if (size < 0 && strcmp(message, MESSAGE) == 0) {
		printf("WebSocket: Received expected message\n");
		success = true;
	} else {
		fprintf(stderr, "Received UNEXPECTED message\n");
		failed = true;
	}
}

static void RTC_API serverOpenCallback(int ws, void *ptr) {
	printf("WebSocketServer: Client connection open\n");

	char path[256];
	if (rtcGetWebSocketPath(ws, path, 256) < 0) {
		fprintf(stderr, "rtcGetWebSocketPath failed\n");
		failed = true;
		return;
	}

	if (strcmp(path, "/mypath") != 0) {
		fprintf(stderr, "Wrong WebSocket path: %s\n", path);
		failed = true;
	}
}

static void RTC_API serverClosedCallback(int ws, void *ptr) {
	printf("WebSocketServer: Client connection closed\n");
}

static void RTC_API serverMessageCallback(int ws, const char *message, int size, void *ptr) {
	if (rtcSendMessage(ws, message, size) < 0) {
		fprintf(stderr, "rtcSendMessage failed\n");
		failed = true;
	}
}

static void RTC_API serverClientCallback(int wsserver, int ws, void *ptr) {
	wsclient = ws;

	char address[256];
	if (rtcGetWebSocketRemoteAddress(ws, address, 256) < 0) {
		fprintf(stderr, "rtcGetWebSocketRemoteAddress failed\n");
		failed = true;
		return;
	}

	printf("WebSocketServer: Received client connection from %s", address);

	rtcSetOpenCallback(ws, serverOpenCallback);
	rtcSetClosedCallback(ws, serverClosedCallback);
	rtcSetMessageCallback(ws, serverMessageCallback);
}

int test_capi_websocketserver_main() {
	const char *url = "wss://localhost:48081/mypath";
	const uint16_t port = 48081;
	int wsserver = -1;
	int ws = -1;
	int attempts;

	rtcInitLogger(RTC_LOG_DEBUG, nullptr);

	rtcWsServerConfiguration serverConfig;
	memset(&serverConfig, 0, sizeof(serverConfig));
	serverConfig.port = port;
	serverConfig.enableTls = true;
	// serverConfig.certificatePemFile = ...
	// serverConfig.keyPemFile = ...

	wsserver = rtcCreateWebSocketServer(&serverConfig, serverClientCallback);
	if (wsserver < 0)
		goto error;

	if (rtcGetWebSocketServerPort(wsserver) != int(port)) {
		fprintf(stderr, "rtcGetWebSocketServerPort failed\n");
		goto error;
	}

	rtcWsConfiguration config;
	memset(&config, 0, sizeof(config));
	config.disableTlsVerification = true;

	ws = rtcCreateWebSocketEx(url, &config);
	if (ws < 0)
		goto error;

	rtcSetOpenCallback(ws, openCallback);
	rtcSetClosedCallback(ws, closedCallback);
	rtcSetMessageCallback(ws, messageCallback);

	attempts = 10;
	while (!success && !failed && attempts--)
		sleep(1);

	if (!success || failed)
		goto error;

	rtcDeleteWebSocket(wsclient);
	sleep(1);

	rtcDeleteWebSocket(ws);
	sleep(1);

	rtcDeleteWebSocketServer(wsserver);
	sleep(1);

	printf("Success\n");
	return 0;

error:
	if (wsclient >= 0)
		rtcDeleteWebSocket(wsclient);

	if (ws >= 0)
		rtcDeleteWebSocket(ws);

	if (wsserver >= 0)
		rtcDeleteWebSocketServer(wsserver);

	return -1;
}

#include <stdexcept>

void test_capi_websocketserver() {
	if (test_capi_websocketserver_main())
		throw std::runtime_error("WebSocketServer test failed");
}

#endif
