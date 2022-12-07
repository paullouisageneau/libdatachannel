/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
