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
#include "internals.hpp"

#include "impl/certificate.hpp"
#include "impl/dtlstransport.hpp"
#include "impl/sctptransport.hpp"
#include "impl/threadpool.hpp"
#include "impl/tls.hpp"

#if RTC_ENABLE_WEBSOCKET
#include "impl/tlstransport.hpp"
#endif

#if RTC_ENABLE_MEDIA
#include "impl/dtlssrtptransport.hpp"
#endif

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rtc {

namespace {

void doInit() {

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData))
		throw std::runtime_error("WSAStartup failed, error=" + std::to_string(WSAGetLastError()));
#endif

	impl::ThreadPool::Instance().spawn(THREADPOOL_SIZE);

#if USE_GNUTLS
	// Nothing to do
#else
	openssl::init();
#endif

	impl::SctpTransport::Init();
	impl::DtlsTransport::Init();
#if RTC_ENABLE_WEBSOCKET
	impl::TlsTransport::Init();
#endif
#if RTC_ENABLE_MEDIA
	impl::DtlsSrtpTransport::Init();
#endif
}

void doCleanup() {
	impl::ThreadPool::Instance().join();

	impl::SctpTransport::Cleanup();
	impl::DtlsTransport::Cleanup();
#if RTC_ENABLE_WEBSOCKET
	impl::TlsTransport::Cleanup();
#endif
#if RTC_ENABLE_MEDIA
	impl::DtlsSrtpTransport::Cleanup();
#endif

#ifdef _WIN32
	WSACleanup();
#endif
}

} // namespace

weak_ptr<void> Init::Weak;
shared_ptr<void> *Init::Global = nullptr;
bool Init::Initialized = false;
SctpSettings Init::CurrentSctpSettings = {};
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
}

void Init::Cleanup() {
	std::unique_lock lock(Mutex);
	delete Global;
	Global = nullptr;
}

void Init::SetSctpSettings(SctpSettings s) {
	auto token = Token();
	std::unique_lock lock(Mutex);
	impl::SctpTransport::SetSettings(s);
	CurrentSctpSettings = std::move(s); // store for next init
}

Init::Init() {
	// Mutex is locked by Token() here
	if (!std::exchange(Initialized, true)) {
		PLOG_DEBUG << "Global initialization";
		doInit();
		impl::SctpTransport::SetSettings(CurrentSctpSettings);
	}
}

Init::~Init() {
	std::thread t([]() {
		// We need to lock Mutex ourselves
		std::unique_lock lock(Mutex);
		if (Global)
			return;

		if (std::exchange(Initialized, false)) {
			PLOG_DEBUG << "Global cleanup";
			doCleanup();
		}
	});
	t.detach();
}

} // namespace rtc
