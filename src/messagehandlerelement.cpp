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

#include "messagehandlerelement.hpp"

namespace rtc {

ChainedMessagesProduct make_chained_messages_product() {
	std::vector<binary_ptr> msgs = {};
	return std::make_shared<std::vector<binary_ptr>>(msgs);
}

ChainedMessagesProduct make_chained_messages_product(message_ptr msg) {
	std::vector<binary_ptr> msgs = {msg};
	return std::make_shared<std::vector<binary_ptr>>(msgs);
}

ChainedOutgoingProduct::ChainedOutgoingProduct(ChainedMessagesProduct messages, std::optional<message_ptr> control)
: messages(messages), control(control) { }

ChainedOutgoingResponseProduct::ChainedOutgoingResponseProduct(std::optional<ChainedMessagesProduct> messages, std::optional<message_ptr> control)
: messages(messages), control(control) { }

ChainedIncomingProduct::ChainedIncomingProduct(std::optional<ChainedMessagesProduct> incoming, std::optional<ChainedMessagesProduct> outgoing)
: incoming(incoming), outgoing(outgoing) { }

ChainedIncomingControlProduct::ChainedIncomingControlProduct(message_ptr incoming, std::optional<ChainedMessagesProduct> outgoing)
: incoming(incoming), outgoing(outgoing) { }

MessageHandlerElement::MessageHandlerElement() { }

void MessageHandlerElement::removeFromChain() {
	if (upstream.has_value()) {
		upstream.value()->downstream = downstream;
	}
	if (downstream.has_value()) {
		downstream.value()->upstream = upstream;
	}
	upstream = nullopt;
	downstream = nullopt;
}

void MessageHandlerElement::recursiveRemoveChain() {
	if (downstream.has_value()) {
		downstream.value()->recursiveRemoveChain();
	}
	removeFromChain();
}

std::optional<ChainedOutgoingResponseProduct> MessageHandlerElement::processOutgoingResponse(ChainedOutgoingResponseProduct messages) {
	if (messages.messages.has_value()) {
		if (upstream.has_value()) {
			auto msgs = upstream.value()->processOutgoingBinary(ChainedOutgoingProduct(messages.messages.value(), messages.control));
			if (msgs.has_value()) {
				auto messages = msgs.value();
				return ChainedOutgoingResponseProduct(std::make_optional(messages.messages), messages.control);
			} else {
				LOG_ERROR << "Generating outgoing control message failed";
				return nullopt;
			}
		} else {
			return messages;
		}
	} else if (messages.control.has_value()) {
		if (upstream.has_value()) {
			auto control = upstream.value()->processOutgoingControl(messages.control.value());
			if (control.has_value()) {
				return ChainedOutgoingResponseProduct(nullopt, control.value());
			} else {
				LOG_ERROR << "Generating outgoing control message failed";
				return nullopt;
			}
		} else {
			return messages;
		}
	} else {
		return ChainedOutgoingResponseProduct();
	}
}

void MessageHandlerElement::prepareAndSendResponse(std::optional<ChainedOutgoingResponseProduct> outgoing, std::function<bool (ChainedOutgoingResponseProduct)> send) {
	if (outgoing.has_value()) {
		auto message = outgoing.value();
		auto response = processOutgoingResponse(message);
		if (response.has_value()) {
			if(!send(response.value())) {
				LOG_DEBUG << "Send failed";
			}
		} else {
			LOG_DEBUG << "No response to send";
		}
	}
}

std::optional<message_ptr> MessageHandlerElement::processIncomingControl(message_ptr message, std::function<bool (ChainedOutgoingResponseProduct)> send) {
	assert(message);
	auto product = modifyIncomingControl(message);
	prepareAndSendResponse(product.outgoing, send);
	if (product.incoming.has_value()) {
		if (downstream.has_value()) {
			if (product.incoming.value()) {
				return downstream.value()->processIncomingControl(product.incoming.value(), send);
			} else {
				LOG_DEBUG << "Failed to generate incoming message";
				return nullopt;
			}
		} else {
			return product.incoming;
		}
	} else {
		return product.incoming;
	}
}

std::optional<ChainedMessagesProduct> MessageHandlerElement::processIncomingBinary(ChainedMessagesProduct messages, std::function<bool (ChainedOutgoingResponseProduct)> send) {
	assert(messages && !messages->empty());
	auto product = modifyIncomingBinary(messages);
	prepareAndSendResponse(product.outgoing, send);
	if (product.incoming.has_value()) {
		if (downstream.has_value()) {
			if (product.incoming.value()) {
				return downstream.value()->processIncomingBinary(product.incoming.value(), send);
			} else {
				LOG_ERROR << "Failed to generate incoming message";
				return nullopt;
			}
		} else {
			return product.incoming;
		}
	} else {
		return product.incoming;
	}
}

std::optional<message_ptr> MessageHandlerElement::processOutgoingControl(message_ptr message) {
	assert(message);
	auto newMessage = modifyOutgoingControl(message);
	assert(newMessage);
	if(!newMessage) {
		LOG_ERROR << "Failed to generate outgoing message";
		return nullopt;
	}
	if (upstream.has_value()) {
		return upstream.value()->processOutgoingControl(newMessage);
	} else {
		return newMessage;
	}
}

std::optional<ChainedOutgoingProduct> MessageHandlerElement::processOutgoingBinary(ChainedOutgoingProduct product) {
	assert(product.messages && !product.messages->empty());
	auto newProduct = modifyOutgoingBinary(product.messages, product.control);
	assert(!product.control.has_value() || newProduct.control.has_value());
	assert(!newProduct.control.has_value() || newProduct.control.value());
	assert(newProduct.messages && !newProduct.messages->empty());
	if (product.control.has_value() && !newProduct.control.has_value()) {
		LOG_ERROR << "Outgoing message must not remove control message";
		return nullopt;
	}
	if (newProduct.control.has_value() && !newProduct.control.value()) {
		LOG_ERROR << "Failed to generate control message";
		return nullopt;
	}
	if (!newProduct.messages || newProduct.messages->empty()) {
		LOG_ERROR << "Failed to generate message";
		return nullopt;
	}
	if (upstream.has_value()) {
		return upstream.value()->processOutgoingBinary(newProduct);
	} else {
		return newProduct;
	}
}

ChainedIncomingControlProduct MessageHandlerElement::modifyIncomingControl(message_ptr messages) {
	return {messages};
}

message_ptr MessageHandlerElement::modifyOutgoingControl(message_ptr messages) {
	return messages;
}

ChainedIncomingProduct MessageHandlerElement::modifyIncomingBinary(ChainedMessagesProduct messages) {
	return {messages};
}

ChainedOutgoingProduct MessageHandlerElement::modifyOutgoingBinary(ChainedMessagesProduct messages, std::optional<message_ptr> control) {
	return {messages, control};
}

std::shared_ptr<MessageHandlerElement> MessageHandlerElement::chainWith(std::shared_ptr<MessageHandlerElement> upstream) {
	assert(this->upstream == nullopt);
	assert(upstream->downstream == nullopt);
	this->upstream = upstream;
	upstream->downstream = shared_from_this();
	return upstream;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
