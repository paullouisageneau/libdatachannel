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

#ifndef RTC_MESSAGE_HANDLER_ELEMENT_H
#define RTC_MESSAGE_HANDLER_ELEMENT_H

#if RTC_ENABLE_MEDIA

#include "include.hpp"
#include "message.hpp"
#include "rtp.hpp"

namespace rtc {

using ChainedMessagesProduct = std::shared_ptr<std::vector<binary_ptr>>;

RTC_CPP_EXPORT ChainedMessagesProduct make_chained_messages_product();
RTC_CPP_EXPORT ChainedMessagesProduct make_chained_messages_product(message_ptr msg);

/// Ougoing messages
struct RTC_CPP_EXPORT ChainedOutgoingProduct {
	ChainedOutgoingProduct(ChainedMessagesProduct messages, std::optional<message_ptr> control = nullopt);
	const ChainedMessagesProduct messages;
	const std::optional<message_ptr> control;
};

/// Ougoing response to incoming messages
struct RTC_CPP_EXPORT ChainedOutgoingResponseProduct {
	ChainedOutgoingResponseProduct(std::optional<ChainedMessagesProduct> messages = nullopt, std::optional<message_ptr> control = nullopt);
	const std::optional<ChainedMessagesProduct> messages;
	const std::optional<message_ptr> control;
};

/// Incoming messages with response
struct RTC_CPP_EXPORT ChainedIncomingProduct {
	ChainedIncomingProduct(std::optional<ChainedMessagesProduct> incoming = nullopt, std::optional<ChainedMessagesProduct> outgoing = nullopt);
	const std::optional<ChainedMessagesProduct> incoming;
	const std::optional<ChainedOutgoingResponseProduct> outgoing;
};

/// Incoming control messages with response
struct RTC_CPP_EXPORT ChainedIncomingControlProduct {
	ChainedIncomingControlProduct(message_ptr incoming, std::optional<ChainedOutgoingResponseProduct> outgoing = nullopt);
	const std::optional<message_ptr> incoming;
	const std::optional<ChainedOutgoingResponseProduct> outgoing;
};

/// Chainable handler
class RTC_CPP_EXPORT MessageHandlerElement: public std::enable_shared_from_this<MessageHandlerElement> {
	std::optional<std::shared_ptr<MessageHandlerElement>> upstream = nullopt;
	std::optional<std::shared_ptr<MessageHandlerElement>> downstream = nullopt;

	void prepareAndSendResponse(std::optional<ChainedOutgoingResponseProduct> outgoing, std::function<bool (ChainedOutgoingResponseProduct)> send);

	void removeFromChain();
public:
	MessageHandlerElement();

	/// Creates response to incoming message
	/// @param messages Current repsonse
	/// @returns New response
	std::optional<ChainedOutgoingResponseProduct> processOutgoingResponse(ChainedOutgoingResponseProduct messages);

	// Process incoming and ougoing messages
	std::optional<message_ptr> formIncomingControlMessage(message_ptr message, std::function<bool (ChainedOutgoingResponseProduct)> send);
	std::optional<ChainedMessagesProduct> formIncomingBinaryMessage(ChainedMessagesProduct messages, std::function<bool (ChainedOutgoingResponseProduct)> send);
	std::optional<message_ptr> formOutgoingControlMessage(message_ptr message);
	std::optional<ChainedOutgoingProduct> formOutgoingBinaryMessage(ChainedOutgoingProduct product);

	/// Process current control message
	/// @param messages current message
	/// @returns Modified message and response
	virtual ChainedIncomingControlProduct processIncomingControlMessage(message_ptr messages);

	/// Process current control message
	/// @param messages current message
	/// @returns Modified message
	virtual message_ptr processOutgoingControlMessage(message_ptr messages);

	/// Process current binary message
	/// @param messages current message
	/// @returns Modified message and response
	virtual ChainedIncomingProduct processIncomingBinaryMessage(ChainedMessagesProduct messages);

	/// Process current binary message
	/// @param messages current message
	/// @param control current control message
	/// @returns Modified binary message and control message
	virtual ChainedOutgoingProduct processOutgoingBinaryMessage(ChainedMessagesProduct messages, std::optional<message_ptr> control);

	/// Set given element as upstream to this
	/// @param upstream Upstream element
	/// @returns Upstream element
	std::shared_ptr<MessageHandlerElement> chainWith(std::shared_ptr<MessageHandlerElement> upstream);

	/// Remove all downstream elements from chain
	void recursiveRemoveChain();
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_MESSAGE_HANDLER_ELEMENT_H
