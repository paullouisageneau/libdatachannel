/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
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

#include "log.hpp"

#include "plog/Appenders/ColorConsoleAppender.h"
#include "plog/Formatters/TxtFormatter.h"
#include "plog/Init.h"
#include "plog/Log.h"
#include "plog/Logger.h"

#include <mutex>

namespace rtc {

void InitLogger(LogLevel level) { InitLogger(static_cast<plog::Severity>(level)); }

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
} // namespace rtc
