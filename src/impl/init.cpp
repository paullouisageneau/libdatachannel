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

struct Init::TokenPayload {
	TokenPayload() { Init::Instance().doInit(); }

	~TokenPayload() {
		std::thread t([]() { Init::Instance().doCleanup(); });
		t.detach();
	}
};

Init &Init::Instance() {
	static Init *instance = new Init;
	return *instance;
}

Init::Init() {}

Init::~Init() {}

init_token Init::token() {
	std::lock_guard lock(mMutex);
	if (auto locked = mWeak.lock())
		return locked;

	mGlobal = std::make_shared<TokenPayload>();
	mWeak = *mGlobal;
	return *mGlobal;
}

void Init::preload() {
	std::lock_guard lock(mMutex);
	if (!mGlobal) {
		mGlobal = std::make_shared<TokenPayload>();
		mWeak = *mGlobal;
	}
}

void Init::cleanup() {
	std::lock_guard lock(mMutex);
	mGlobal.reset();
}

void Init::setSctpSettings(SctpSettings s) {
	std::lock_guard lock(mMutex);
	if (mGlobal)
		impl::SctpTransport::SetSettings(s);

	mCurrentSctpSettings = std::move(s); // store for next init
}

void Init::doInit() {
	// mMutex needs to be locked

	if (std::exchange(mInitialized, true))
		return;

	PLOG_DEBUG << "Global initialization";

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
	impl::SctpTransport::SetSettings(mCurrentSctpSettings);
	impl::DtlsTransport::Init();
#if RTC_ENABLE_WEBSOCKET
	impl::TlsTransport::Init();
#endif
#if RTC_ENABLE_MEDIA
	impl::DtlsSrtpTransport::Init();
#endif
}

void Init::doCleanup() {
	std::lock_guard lock(mMutex);
	if (mGlobal)
		return;

	if (!std::exchange(mInitialized, false))
		return;

	PLOG_DEBUG << "Global cleanup";

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

} // namespace rtc
