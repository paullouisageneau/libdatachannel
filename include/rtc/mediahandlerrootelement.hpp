/**
 * Copyright (c) 2020 Filip Klembara (in2core)
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

#ifndef RTC_MEDIA_HANDLER_ROOT_ELEMENT_H
#define RTC_MEDIA_HANDLER_ROOT_ELEMENT_H

#if RTC_ENABLE_MEDIA

#include "mediahandlerelement.hpp"

namespace rtc {

/// Chainable message handler
class RTC_CPP_EXPORT MediaHandlerRootElement : public MediaHandlerElement {
public:
	MediaHandlerRootElement() {}

	/// Reduce multiple messages into one message
	/// @param messages Messages to reduce
	virtual message_ptr reduce(ChainedMessagesProduct messages);

	/// Splits message into multiple messages
	/// @param message Message to split
	virtual ChainedMessagesProduct split(message_ptr message);
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_MEDIA_HANDLER_ROOT_ELEMENT_H
