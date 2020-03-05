/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#ifndef RTC_LOG_H
#define RTC_LOG_H

#include "plog/Appenders/ColorConsoleAppender.h"
#include "plog/Log.h"
#include "plog/Logger.h"

namespace rtc {

enum class LogLevel { // Don't change, it must match plog severity
	None = 0,
	Fatal = 1,
	Error = 2,
	Warning = 3,
	Info = 4,
	Debug = 5,
	Verbose = 6
};

inline void InitLogger(plog::Severity severity, plog::IAppender *appender = nullptr) {
	static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
	static plog::Logger<0> *logger = nullptr;
	if (!logger) {
		logger = &plog::init(severity, appender ? appender : &consoleAppender);
		PLOG_DEBUG << "Logger initialized";
	} else {
		logger->setMaxSeverity(severity);
		if (appender)
			logger->addAppender(appender);
	}
}

inline void InitLogger(LogLevel level) { InitLogger(static_cast<plog::Severity>(level)); }

}

#endif
