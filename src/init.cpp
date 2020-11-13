/**
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#include "init.hpp"

#include "certificate.hpp"
#include "dtlstransport.hpp"
#include "sctptransport.hpp"
#include "threadpool.hpp"
#include "tls.hpp"

#if RTC_ENABLE_WEBSOCKET
#include "tlstransport.hpp"
#endif

#if RTC_ENABLE_MEDIA
#include "dtlssrtptransport.hpp"
#endif

#ifdef _WIN32
#include <winsock2.h>
#endif

using std::shared_ptr;

namespace rtc {

namespace {

void doInit() {
	PLOG_DEBUG << "Global initialization";

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData))
		throw std::runtime_error("WSAStartup failed, error=" + std::to_string(WSAGetLastError()));
#endif

	ThreadPool::Instance().spawn(THREADPOOL_SIZE);

#if USE_GNUTLS
	// Nothing to do
#else
	openssl::init();
#endif

	SctpTransport::Init();
	DtlsTransport::Init();
#if RTC_ENABLE_WEBSOCKET
	TlsTransport::Init();
#endif
#if RTC_ENABLE_MEDIA
	DtlsSrtpTransport::Init();
#endif
}

void doCleanup() {
	PLOG_DEBUG << "Global cleanup";

	ThreadPool::Instance().join();
	CleanupCertificateCache();

	SctpTransport::Cleanup();
	DtlsTransport::Cleanup();
#if RTC_ENABLE_WEBSOCKET
	TlsTransport::Cleanup();
#endif
#if RTC_ENABLE_MEDIA
	DtlsSrtpTransport::Cleanup();
#endif

#ifdef _WIN32
	WSACleanup();
#endif
}

} // namespace

std::weak_ptr<void> Init::Weak;
std::shared_ptr<void> *Init::Global = nullptr;
bool Init::Initialized = false;
std::recursive_mutex Init::Mutex;

init_token Init::Token() {
	std::unique_lock lock(Mutex);
	if (auto token = Weak.lock())
		return token;

	delete Global;
	Global = new shared_ptr<void>(new Init());
	Weak = *Global;
	return *Global;
}

void Init::Preload() {
	std::unique_lock lock(Mutex);
	auto token = Token();
	if (!Global)
		Global = new shared_ptr<void>(token);

	PLOG_DEBUG << "Preloading certificate";
	make_certificate().wait();
}

void Init::Cleanup() {
	std::unique_lock lock(Mutex);
	delete Global;
	Global = nullptr;
}

Init::Init() {
	// Mutex is locked by Token() here
	if (!std::exchange(Initialized, true))
		doInit();
}

Init::~Init() {
	std::thread t([]() {
		// We need to lock Mutex ourselves
		std::unique_lock lock(Mutex);
		if (Global)
			return;
		if (std::exchange(Initialized, false))
			doCleanup();
	});
	t.detach();
}

} // namespace rtc
