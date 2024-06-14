/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "plog/Appenders/ColorConsoleAppender.h"
#include "plog/Converters/UTF8Converter.h"
#include "plog/Formatters/FuncMessageFormatter.h"
#include "plog/Formatters/TxtFormatter.h"
#include "plog/Init.h"
#include "plog/Log.h"
#include "plog/Logger.h"
//
#include "global.hpp"

#include "impl/init.hpp"

#include <mutex>
#include <map>

#if !USE_NICE
#include <juice/juice.h>
#endif

namespace {

void plogInit(plog::Severity severity, plog::IAppender *appender) {
	using Logger = plog::Logger<PLOG_DEFAULT_INSTANCE_ID>;
	static Logger *logger = nullptr;
	if (!logger) {
		PLOG_DEBUG << "Initializing logger";
		logger = new Logger(severity);
		if (appender) {
			logger->addAppender(appender);
		} else {
			using ConsoleAppender = plog::ColorConsoleAppender<plog::TxtFormatter>;
			static ConsoleAppender *consoleAppender = new ConsoleAppender();
			logger->addAppender(consoleAppender);
		}
	} else {
		logger->setMaxSeverity(severity);
		if (appender)
			logger->addAppender(appender);
	}
}

} // namespace

namespace rtc {

struct LogAppender : public plog::IAppender {
	synchronized_callback<LogLevel, string> callback;

	void write(const plog::Record &record) override {
		const auto severity = record.getSeverity();
		auto formatted = plog::FuncMessageFormatter::format(record);
		formatted.pop_back(); // remove newline

		const auto &converted =
		    plog::UTF8Converter::convert(formatted); // does nothing on non-Windows systems

		if (!callback(static_cast<LogLevel>(severity), converted))
			std::cout << plog::severityToString(severity) << " " << converted << std::endl;
	}
};

void InitLogger(LogLevel level, LogCallback callback) {
	const auto severity = static_cast<plog::Severity>(level);
	static LogAppender *appender = nullptr;
	static std::mutex mutex;
	std::lock_guard lock(mutex);
	if (appender) {
		appender->callback = std::move(callback);
		plogInit(severity, nullptr); // change the severity
	} else if (callback) {
		appender = new LogAppender();
		appender->callback = std::move(callback);
		plogInit(severity, appender);
	} else {
		plogInit(severity, nullptr); // log to cout
	}
}

void InitLogger(plog::Severity severity, plog::IAppender *appender) {
	plogInit(severity, appender);
}

void Preload() { impl::Init::Instance().preload(); }
std::shared_future<void> Cleanup() { return impl::Init::Instance().cleanup(); }

void SetSctpSettings(SctpSettings s) { impl::Init::Instance().setSctpSettings(std::move(s)); }

#if !USE_NICE

UnhandledStunRequestCallback unboundStunCallback;

void InvokeUnhandledStunRequestCallback (const juice_mux_binding_request *info, void *user_ptr) {
	PLOG_DEBUG << "Invoking Unbind STUN listener";
	auto callback = static_cast<UnhandledStunRequestCallback *>(user_ptr);

	(*callback)({
		std::string(info->local_ufrag),
		std::string(info->remote_ufrag),
		std::string(info->address),
		info->port
	});
}

#endif

void OnUnhandledStunRequest ([[maybe_unused]] std::string host, [[maybe_unused]] int port, [[maybe_unused]] UnhandledStunRequestCallback callback) {
	#if USE_NICE
		PLOG_WARNING << "BindStunListener is not supported with libnice, please use libjuice";
	#else
	if (callback == NULL) {
		PLOG_DEBUG << "Removing unhandled STUN request listener";

		// call with NULL callback to unbind
		if (juice_mux_listen(host.c_str(), port, NULL, NULL) < 0) {
			throw std::runtime_error("Could not unbind STUN listener");
		}
		unboundStunCallback = NULL;

		return;
	}

	PLOG_DEBUG << "Adding listener for unhandled STUN requests";

	if (unboundStunCallback != NULL) {
		throw std::runtime_error("Unhandled STUN request handler already present");
	}

	unboundStunCallback = std::move(callback);

	if (juice_mux_listen(host.c_str(), port, &InvokeUnhandledStunRequestCallback, &unboundStunCallback) < 0) {
		throw std::invalid_argument("Could add listener for unhandled STUN requests");
	}
	#endif
}

RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, LogLevel level) {
	switch (level) {
	case LogLevel::Fatal:
		out << "fatal";
		break;
	case LogLevel::Error:
		out << "error";
		break;
	case LogLevel::Warning:
		out << "warning";
		break;
	case LogLevel::Info:
		out << "info";
		break;
	case LogLevel::Debug:
		out << "debug";
		break;
	case LogLevel::Verbose:
		out << "verbose";
		break;
	default:
		out << "none";
		break;
	}
	return out;
}

} // namespace rtc
