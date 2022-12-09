/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "mediahandlerelement.hpp"

#include "impl/internals.hpp"

#include <cassert>

namespace rtc {

ChainedMessagesProduct make_chained_messages_product() {
	return std::make_shared<std::vector<binary_ptr>>();
}

ChainedMessagesProduct make_chained_messages_product(message_ptr msg) {
	std::vector<binary_ptr> msgs = {msg};
	return std::make_shared<std::vector<binary_ptr>>(msgs);
}

ChainedOutgoingProduct::ChainedOutgoingProduct(ChainedMessagesProduct messages, message_ptr control)
    : messages(messages), control(control) {}

ChainedIncomingProduct::ChainedIncomingProduct(ChainedMessagesProduct incoming,
                                               ChainedMessagesProduct outgoing)
    : incoming(incoming), outgoing(outgoing) {}

ChainedIncomingControlProduct::ChainedIncomingControlProduct(
    message_ptr incoming, optional<ChainedOutgoingProduct> outgoing)
    : incoming(incoming), outgoing(outgoing) {}

MediaHandlerElement::MediaHandlerElement() {}

void MediaHandlerElement::removeFromChain() {
	if (upstream) {
		upstream->downstream = downstream;
	}
	if (downstream) {
		downstream->upstream = upstream;
	}
	upstream = nullptr;
	downstream = nullptr;
}

void MediaHandlerElement::recursiveRemoveChain() {
	if (downstream) {
		// `recursiveRemoveChain` removes last strong reference to downstream element
		// we need to keep strong reference to prevent deallocation of downstream element
		// during `recursiveRemoveChain`
		auto strongDownstreamPtr = downstream;
		downstream->recursiveRemoveChain();
	}
	removeFromChain();
}

optional<ChainedOutgoingProduct>
MediaHandlerElement::processOutgoingResponse(ChainedOutgoingProduct messages) {
	if (messages.messages) {
		if (upstream) {
			auto msgs = upstream->formOutgoingBinaryMessage(
			    ChainedOutgoingProduct(messages.messages, messages.control));
			if (msgs.has_value()) {
				return msgs.value();
			} else {
				LOG_ERROR << "Generating outgoing message failed";
				return nullopt;
			}
		} else {
			return messages;
		}
	} else if (messages.control) {
		if (upstream) {
			auto control = upstream->formOutgoingControlMessage(messages.control);
			if (control) {
				return ChainedOutgoingProduct(nullptr, control);
			} else {
				LOG_ERROR << "Generating outgoing control message failed";
				return nullopt;
			}
		} else {
			return messages;
		}
	} else {
		return ChainedOutgoingProduct();
	}
}

void MediaHandlerElement::prepareAndSendResponse(optional<ChainedOutgoingProduct> outgoing,
                                                 std::function<bool(ChainedOutgoingProduct)> send) {
	if (outgoing.has_value()) {
		auto message = outgoing.value();
		auto response = processOutgoingResponse(message);
		if (response.has_value()) {
			if (!send(response.value())) {
				LOG_DEBUG << "Send failed";
			}
		} else {
			LOG_DEBUG << "No response to send";
		}
	}
}

message_ptr
MediaHandlerElement::formIncomingControlMessage(message_ptr message,
                                                std::function<bool(ChainedOutgoingProduct)> send) {
	assert(message);
	auto product = processIncomingControlMessage(message);
	prepareAndSendResponse(product.outgoing, send);
	if (product.incoming) {
		if (downstream) {
			return downstream->formIncomingControlMessage(product.incoming, send);
		} else {
			return product.incoming;
		}
	} else {
		return nullptr;
	}
}

ChainedMessagesProduct
MediaHandlerElement::formIncomingBinaryMessage(ChainedMessagesProduct messages,
                                               std::function<bool(ChainedOutgoingProduct)> send) {
	assert(messages && !messages->empty());
	auto product = processIncomingBinaryMessage(messages);
	prepareAndSendResponse(product.outgoing, send);
	if (product.incoming) {
		if (downstream) {
			return downstream->formIncomingBinaryMessage(product.incoming, send);
		} else {
			return product.incoming;
		}
	} else {
		return nullptr;
	}
}

message_ptr MediaHandlerElement::formOutgoingControlMessage(message_ptr message) {
	assert(message);
	auto newMessage = processOutgoingControlMessage(message);
	assert(newMessage);
	if (!newMessage) {
		LOG_ERROR << "Failed to generate outgoing message";
		return nullptr;
	}
	if (upstream) {
		return upstream->formOutgoingControlMessage(newMessage);
	} else {
		return newMessage;
	}
}

optional<ChainedOutgoingProduct>
MediaHandlerElement::formOutgoingBinaryMessage(ChainedOutgoingProduct product) {
	assert(product.messages && !product.messages->empty());
	auto newProduct = processOutgoingBinaryMessage(product.messages, product.control);
	assert(!product.control || newProduct.control);
	assert(newProduct.messages && !newProduct.messages->empty());
	if (product.control && !newProduct.control) {
		LOG_ERROR << "Outgoing message must not remove control message";
		return nullopt;
	}
	if (!newProduct.messages || newProduct.messages->empty()) {
		LOG_ERROR << "Failed to generate message";
		return nullopt;
	}
	if (upstream) {
		return upstream->formOutgoingBinaryMessage(newProduct);
	} else {
		return newProduct;
	}
}

ChainedIncomingControlProduct
MediaHandlerElement::processIncomingControlMessage(message_ptr messages) {
	return {messages};
}

message_ptr MediaHandlerElement::processOutgoingControlMessage(message_ptr messages) {
	return messages;
}

ChainedIncomingProduct
MediaHandlerElement::processIncomingBinaryMessage(ChainedMessagesProduct messages) {
	return {messages};
}

ChainedOutgoingProduct
MediaHandlerElement::processOutgoingBinaryMessage(ChainedMessagesProduct messages,
                                                  message_ptr control) {
	return {messages, control};
}

shared_ptr<MediaHandlerElement>
MediaHandlerElement::chainWith(shared_ptr<MediaHandlerElement> upstream) {
	assert(this->upstream == nullptr);
	assert(upstream->downstream == nullptr);
	this->upstream = upstream;
	upstream->downstream = shared_from_this();
	return upstream;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
