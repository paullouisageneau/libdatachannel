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

#if RTC_ENABLE_MEDIA

#include "mediachainablehandler.hpp"

#include "impl/internals.hpp"

#include <cassert>

namespace rtc {

MediaChainableHandler::MediaChainableHandler(shared_ptr<MediaHandlerRootElement> root): MediaHandler(), root(root), leaf(root) { }

MediaChainableHandler::~MediaChainableHandler() {
	leaf->recursiveRemoveChain();
}

bool MediaChainableHandler::sendProduct(ChainedOutgoingProduct product) {
	bool result = true;
	if (product.control) {
		assert(product.control->type == Message::Control);
		auto sendResult = send(product.control);
		if(!sendResult) {
			LOG_DEBUG << "Failed to send control message";
		}
		result = result && sendResult;
	}
	if (product.messages) {
		auto messages = product.messages;
		for (unsigned i = 0; i < messages->size(); i++) {
			auto message = messages->at(i);
			if (!message) {
				LOG_DEBUG << "Invalid message to send " << i + 1 << "/" << messages->size();
			}
			auto sendResult = send(make_message(*message));
			if(!sendResult) {
				LOG_DEBUG << "Failed to send message " << i + 1 << "/" << messages->size();
			}
			result = result && sendResult;
		}
	}
	return result;
}

message_ptr MediaChainableHandler::handleIncomingBinary(message_ptr msg) {
	assert(msg->type == Message::Binary);
	auto messages = root->split(msg);
	auto incoming = getLeaf()->formIncomingBinaryMessage(
	    messages, [this](ChainedOutgoingProduct outgoing) { return sendProduct(outgoing); });
	if (incoming) {
		return root->reduce(incoming);
	} else {
		return nullptr;
	}
}

message_ptr MediaChainableHandler::handleIncomingControl(message_ptr msg) {
	assert(msg->type == Message::Control);
	auto incoming = getLeaf()->formIncomingControlMessage(
	    msg, [this](ChainedOutgoingProduct outgoing) { return sendProduct(outgoing); });
	assert(!incoming || incoming->type == Message::Control);
	return incoming;
}

message_ptr MediaChainableHandler::handleOutgoingBinary(message_ptr msg) {
	assert(msg->type == Message::Binary);
	auto messages = make_chained_messages_product(msg);
	auto optOutgoing = root->formOutgoingBinaryMessage(ChainedOutgoingProduct(messages));
	if (!optOutgoing.has_value()) {
		LOG_ERROR << "Generating outgoing message failed";
		return nullptr;
	}
	auto outgoing = optOutgoing.value();
	if (outgoing.control) {
		if(!send(outgoing.control)) {
			LOG_DEBUG << "Failed to send control message";
		}
	}
	auto lastMessage = outgoing.messages->back();
	if (!lastMessage) {
		LOG_DEBUG << "Invalid message to send";
		return nullptr;
	}
	for (unsigned i = 0; i < outgoing.messages->size() - 1; i++) {
		auto message = outgoing.messages->at(i);
		if (!message) {
			LOG_DEBUG << "Invalid message to send " << i + 1 << "/" << outgoing.messages->size();
		}
		if(!send(make_message(*message))) {
			LOG_DEBUG << "Failed to send message " << i + 1 << "/" << outgoing.messages->size();
		}
	}
	return make_message(*lastMessage);
}

message_ptr MediaChainableHandler::handleOutgoingControl(message_ptr msg) {
	assert(msg->type == Message::Control);
	auto outgoing = root->formOutgoingControlMessage(msg);
	assert(!outgoing || outgoing->type == Message::Control);
	if (!outgoing) {
		LOG_ERROR << "Generating outgoing control message failed";
		return nullptr;
	}
	return outgoing;
}

message_ptr MediaChainableHandler::outgoing(message_ptr ptr) {
	assert(ptr);
	if (!ptr) {
		LOG_ERROR << "Outgoing message is nullptr, ignoring";
		return nullptr;
	}
	if (ptr->type == Message::Binary) {
		return handleOutgoingBinary(ptr);
	} else if (ptr->type == Message::Control) {
		return handleOutgoingControl(ptr);
	}
	return ptr;
}

message_ptr MediaChainableHandler::incoming(message_ptr ptr) {
	if (!ptr) {
		LOG_ERROR << "Incoming message is nullptr, ignoring";
		return nullptr;
	}
	if (ptr->type == Message::Binary) {
		return handleIncomingBinary(ptr);
	} else if (ptr->type == Message::Control) {
		return handleIncomingControl(ptr);
	}
	return ptr;
}

bool MediaChainableHandler::send(message_ptr msg) {
	try {
		outgoingCallback(std::move(msg));
		return true;
	} catch (const std::exception &e) {
		LOG_DEBUG << "Send in RTCP chain handler failed: " << e.what();
	}
	return false;
}

shared_ptr<MediaHandlerElement> MediaChainableHandler::getLeaf() const {
	std::lock_guard lock(mutex);
	return leaf;
}

void MediaChainableHandler::addToChain(shared_ptr<MediaHandlerElement> chainable) {
	std::lock_guard lock(mutex);
	assert(leaf);
	leaf = leaf->chainWith(chainable);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
