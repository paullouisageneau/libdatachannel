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

#ifndef RTC_MEDIA_CHAINABLE_HANDLER_H
#define RTC_MEDIA_CHAINABLE_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "mediahandlerrootelement.hpp"

namespace rtc {

class RTC_CPP_EXPORT MediaChainableHandler : public MediaHandler {
	const shared_ptr<MediaHandlerRootElement> root;
	shared_ptr<MediaHandlerElement> leaf;
	mutable std::mutex mutex;

	message_ptr handleIncomingBinary(message_ptr);
	message_ptr handleIncomingControl(message_ptr);
	message_ptr handleOutgoingBinary(message_ptr);
	message_ptr handleOutgoingControl(message_ptr);
	bool sendProduct(ChainedOutgoingProduct product);
	shared_ptr<MediaHandlerElement> getLeaf() const;

public:
	MediaChainableHandler(shared_ptr<MediaHandlerRootElement> root);
	~MediaChainableHandler();
	message_ptr incoming(message_ptr ptr) override;
	message_ptr outgoing(message_ptr ptr) override;

	bool send(message_ptr msg);

	/// Adds element to chain
	/// @param chainable Chainable element
	void addToChain(shared_ptr<MediaHandlerElement> chainable);
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_MEDIA_CHAINABLE_HANDLER_H
