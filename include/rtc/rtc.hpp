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

// C API
#include "rtc.h"

// C++ API
#include "common.hpp"
#include "global.hpp"
//
#include "datachannel.hpp"
#include "peerconnection.hpp"
#include "track.hpp"

#if RTC_ENABLE_WEBSOCKET

// WebSocket
#include "websocket.hpp"
#include "websocketserver.hpp"

#endif // RTC_ENABLE_WEBSOCKET

#if RTC_ENABLE_MEDIA

// Media handling
#include "mediachainablehandler.hpp"
#include "rtcpnackresponder.hpp"
#include "rtcpreceivingsession.hpp"
#include "rtcpsrreporter.hpp"

// Opus/h264 streaming
#include "h264packetizationhandler.hpp"
#include "opuspacketizationhandler.hpp"

#endif // RTC_ENABLE_MEDIA
