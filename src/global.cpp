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

#include "plog/Appenders/ColorConsoleAppender.h"
#include "plog/Formatters/FuncMessageFormatter.h"
#include "plog/Formatters/TxtFormatter.h"
#include "plog/Init.h"
#include "plog/Log.h"
#include "plog/Logger.h"
//
#include "global.hpp"

#include "impl/init.hpp"

#include <mutex>

#ifdef _WIN32
#include <codecvt>
#include <locale>
#endif

namespace rtc {

struct LogAppender : public plog::IAppender {
	synchronized_callback<LogLevel, string> callback;

	void write(const plog::Record &record) override {
		const auto severity = record.getSeverity();
		auto formatted = plog::FuncMessageFormatter::format(record);
		formatted.pop_back(); // remove newline

#ifdef _WIN32
		using convert_type = std::codecvt_utf8<wchar_t>;
		std::wstring_convert<convert_type, wchar_t> converter;
		std::string str = converter.to_bytes(formatted);
#else
		std::string str = formatted;
#endif

		if (!callback(static_cast<LogLevel>(severity), str))
			std::cout << plog::severityToString(severity) << " " << str << std::endl;
	}
};

void InitLogger(LogLevel level, LogCallback callback) {
	static unique_ptr<LogAppender> appender;
	const auto severity = static_cast<plog::Severity>(level);
	if (appender) {
		appender->callback = std::move(callback);
		InitLogger(severity, nullptr); // change the severity
	} else if (callback) {
		appender = std::make_unique<LogAppender>();
		appender->callback = std::move(callback);
		InitLogger(severity, appender.get());
	} else {
		InitLogger(severity, nullptr); // log to cout
	}
}

void InitLogger(plog::Severity severity, plog::IAppender *appender) {
	static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
	static plog::Logger<0> *logger = nullptr;
	static std::mutex mutex;
	std::lock_guard lock(mutex);
	if (!logger) {
		logger = &plog::init(severity, appender ? appender : &consoleAppender);
		PLOG_DEBUG << "Logger initialized";
	} else {
		logger->setMaxSeverity(severity);
		if (appender)
			logger->addAppender(appender);
	}
}

void Preload() { Init::Preload(); }
void Cleanup() { Init::Cleanup(); }

void SetSctpSettings(SctpSettings s) { Init::SetSctpSettings(std::move(s)); }

} // namespace rtc

RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, rtc::LogLevel level) {
	switch (level) {
	case rtc::LogLevel::Fatal:
		out << "fatal";
		break;
	case rtc::LogLevel::Error:
		out << "error";
		break;
	case rtc::LogLevel::Warning:
		out << "warning";
		break;
	case rtc::LogLevel::Info:
		out << "info";
		break;
	case rtc::LogLevel::Debug:
		out << "debug";
		break;
	case rtc::LogLevel::Verbose:
		out << "verbose";
		break;
	default:
		out << "none";
		break;
	}
	return out;
}

