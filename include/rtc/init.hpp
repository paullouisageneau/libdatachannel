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

#ifndef RTC_INIT_H
#define RTC_INIT_H

#include "include.hpp"

#include <mutex>

namespace rtc {

class Init;
using init_token = std::shared_ptr<Init>;

class Init {
public:
	static init_token Token();
	static void Preload();
	static void Cleanup();

	~Init();

private:
	Init();

	static std::weak_ptr<Init> Weak;
	static init_token Global;
	static std::mutex Mutex;
};

inline void Preload() { Init::Preload(); }
inline void Cleanup() { Init::Cleanup(); }

} // namespace rtc

#endif
