/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#if RTC_ENABLE_MEDIA

#include "mediahandlerrootelement.hpp"

namespace rtc {

message_ptr MediaHandlerRootElement::reduce(ChainedMessagesProduct messages) {
	if (messages && !messages->empty()) {
		auto msg_ptr = messages->front();
		if (msg_ptr) {
			return make_message(*msg_ptr);
		} else {
			return nullptr;
		}
	} else {
		return nullptr;
	}
}

ChainedMessagesProduct MediaHandlerRootElement::split(message_ptr message) {
	return make_chained_messages_product(message);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
