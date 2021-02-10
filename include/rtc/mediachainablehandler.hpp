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

#ifndef RTC_MEDIA_CHAINABLE_HANDLER_H
#define RTC_MEDIA_CHAINABLE_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "mediahandlerrootelement.hpp"

namespace rtc {

class RTC_CPP_EXPORT MediaChainableHandler : public MediaHandler {
	const std::shared_ptr<MediaHandlerRootElement> root;
	std::shared_ptr<MediaHandlerElement> leaf;
	std::mutex inoutMutex;

	message_ptr handleIncomingBinary(message_ptr);
	message_ptr handleIncomingControl(message_ptr);
	message_ptr handleOutgoingBinary(message_ptr);
	message_ptr handleOutgoingControl(message_ptr);
	bool sendProduct(ChainedOutgoingProduct product);
public:
	MediaChainableHandler(std::shared_ptr<MediaHandlerRootElement> root);
	~MediaChainableHandler();
	message_ptr incoming(message_ptr ptr) override;
	message_ptr outgoing(message_ptr ptr) override;

	bool send(message_ptr msg);

	/// Adds element to chain
	/// @param chainable Chainable element
    void addToChain(std::shared_ptr<MediaHandlerElement> chainable);
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_MEDIA_CHAINABLE_HANDLER_H
