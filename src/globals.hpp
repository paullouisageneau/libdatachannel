/**
 * Copyright (c) 2019-2021 Paul-Louis Ageneau
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

#ifndef RTC_GLOBALS_H
#define RTC_GLOBALS_H

#include "common.hpp"

namespace rtc {

const size_t MAX_NUMERICNODE_LEN = 48; // Max IPv6 string representation length
const size_t MAX_NUMERICSERV_LEN = 6;  // Max port string representation length

const uint16_t DEFAULT_SCTP_PORT = 5000;          // SCTP port to use by default
const size_t DEFAULT_MAX_MESSAGE_SIZE = 65536;    // Remote max message size if not specified in SDP
const size_t LOCAL_MAX_MESSAGE_SIZE = 256 * 1024; // Local max message size

const size_t RECV_QUEUE_LIMIT = 1024 * 1024; // Max per-channel queue size

const int THREADPOOL_SIZE = 4; // Number of threads in the global thread pool (>= 2)

const size_t DEFAULT_IPV4_MTU = 1200; // IPv4 safe MTU value recommended by RFC 8261

} // namespace rtc

#endif
