/**
 * Copyright (c) 2020-2022 Paul-Louis Ageneau
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

#include "certificate.hpp"
#include "dtlstransport.hpp"
#include "pollservice.hpp"
#include "sctptransport.hpp"
#include "icetransport.hpp"
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

namespace rtc::impl {

struct Init::TokenPayload {
	TokenPayload(std::shared_future<void> *cleanupFuture) {
		Init::Instance().doInit();
		if (cleanupFuture)
			*cleanupFuture = cleanupPromise.get_future().share();
	}

	~TokenPayload() {
		std::thread t(
		    [](std::promise<void> promise) {
			    try {
				    Init::Instance().doCleanup();
				    promise.set_value();
			    } catch (const std::exception &e) {
				    PLOG_WARNING << e.what();
				    promise.set_exception(std::make_exception_ptr(e));
			    }
		    },
		    std::move(cleanupPromise));
		t.detach();
	}

	std::promise<void> cleanupPromise;
};

Init &Init::Instance() {
	static Init *instance = new Init;
	return *instance;
}

Init::Init() {
	std::promise<void> p;
	p.set_value();
	mCleanupFuture = p.get_future(); // make it ready
}

Init::~Init() {}

init_token Init::token() {
	std::lock_guard lock(mMutex);
	if (auto locked = mWeak.lock())
		return locked;

	mGlobal = std::make_shared<TokenPayload>(&mCleanupFuture);
	mWeak = *mGlobal;
	return *mGlobal;
}

void Init::preload() {
	std::lock_guard lock(mMutex);
	if (!mGlobal) {
		mGlobal = std::make_shared<TokenPayload>(&mCleanupFuture);
		mWeak = *mGlobal;
	}
}

std::shared_future<void> Init::cleanup() {
	std::lock_guard lock(mMutex);
	mGlobal.reset();
	return mCleanupFuture;
}

void Init::setSctpSettings(SctpSettings s) {
	std::lock_guard lock(mMutex);
	if (mGlobal)
		SctpTransport::SetSettings(s);

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

	ThreadPool::Instance().spawn(THREADPOOL_SIZE);
#if RTC_ENABLE_WEBSOCKET
	PollService::Instance().start();
#endif

#if USE_GNUTLS
	// Nothing to do
#else
	openssl::init();
#endif

	SctpTransport::Init();
	SctpTransport::SetSettings(mCurrentSctpSettings);
	DtlsTransport::Init();
#if RTC_ENABLE_WEBSOCKET
	TlsTransport::Init();
#endif
#if RTC_ENABLE_MEDIA
	DtlsSrtpTransport::Init();
#endif
}

void Init::doCleanup() {
	std::lock_guard lock(mMutex);
	if (mGlobal)
		return;

	if (!std::exchange(mInitialized, false))
		return;

	PLOG_DEBUG << "Global cleanup";

	ThreadPool::Instance().join();
#if RTC_ENABLE_WEBSOCKET
	PollService::Instance().join();
#endif

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

} // namespace rtc::impl
