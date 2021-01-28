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

#include "rtcpchainablehandler.hpp"

namespace rtc {

RtcpChainableHandler::RtcpChainableHandler(std::shared_ptr<MessageHandlerRootElement> root): root(root), leaf(root) { }

RtcpChainableHandler::~RtcpChainableHandler() {
	leaf->recursiveRemoveChain();
}

bool RtcpChainableHandler::sendProduct(ChainedOutgoingResponseProduct product) {
	bool result = true;
	if (product.control.has_value()) {
		auto sendResult = send(product.control.value());
		if(!sendResult) {
			LOG_DEBUG << "Failed to send control message";
		}
		result = result && sendResult;
	}
	if (product.messages.has_value()) {
		auto messages = product.messages.value();
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

std::optional<message_ptr> RtcpChainableHandler::handleIncomingBinary(message_ptr msg) {
	assert(msg->type == Message::Binary);
	auto messages = root->split(msg);
	auto incoming = leaf->processIncomingBinary(messages, [this](ChainedOutgoingResponseProduct outgoing) {
		return sendProduct(outgoing);
	});
	if (incoming.has_value()) {
		return root->reduce(incoming.value());
	} else {
		return nullopt;
	}
}

std::optional<message_ptr> RtcpChainableHandler::handleIncomingControl(message_ptr msg) {
	assert(msg->type == Message::Control);
	auto incoming = leaf->processIncomingControl(msg, [this](ChainedOutgoingResponseProduct outgoing) {
		return sendProduct(outgoing);
	});
	assert(!incoming.has_value() || incoming.value()->type == Message::Control);
	return incoming;
}

std::optional<message_ptr> RtcpChainableHandler::handleOutgoingBinary(message_ptr msg) {
	assert(msg->type == Message::Binary);
	auto messages = make_chained_messages_product(msg);
	auto optOutgoing = root->processOutgoingBinary(ChainedOutgoingProduct(messages));
	if (!optOutgoing.has_value()) {
		LOG_ERROR << "Generating outgoing message failed";
		return nullopt;
	}
	auto outgoing = optOutgoing.value();
	if (outgoing.control.has_value()) {
		if(!send(outgoing.control.value())) {
			LOG_DEBUG << "Failed to send control message";
		}
	}
	auto lastMessage = outgoing.messages->back();
	if (!lastMessage) {
		LOG_DEBUG << "Invalid message to send";
		return nullopt;
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

std::optional<message_ptr> RtcpChainableHandler::handleOutgoingControl(message_ptr msg) {
	assert(msg->type == Message::Control);
	auto optOutgoing = root->processOutgoingControl(msg);
	assert(!optOutgoing.has_value() || optOutgoing.value()->type == Message::Control);
	if (!optOutgoing.has_value()) {
		LOG_ERROR << "Generating outgoing control message failed";
		return nullopt;
	}
	return optOutgoing.value();
}

message_ptr RtcpChainableHandler::outgoing(message_ptr ptr) {
	assert(ptr);
	if (!ptr) {
		LOG_ERROR << "Outgoing message is nullptr, ignoring";
		return nullptr;
	}
	std::lock_guard<std::mutex> guard(inoutMutex);
	if (ptr->type == Message::Binary) {
		return handleOutgoingBinary(ptr).value_or(nullptr);
	} else if (ptr->type == Message::Control) {
		return handleOutgoingControl(ptr).value_or(nullptr);
	}
	return ptr;
}

message_ptr RtcpChainableHandler::incoming(message_ptr ptr) {
	if (!ptr) {
		LOG_ERROR << "Incoming message is nullptr, ignoring";
		return nullptr;
	}
	std::lock_guard<std::mutex> guard(inoutMutex);
	if (ptr->type == Message::Binary) {
		return handleIncomingBinary(ptr).value_or(nullptr);
	} else if (ptr->type == Message::Control) {
		return handleIncomingControl(ptr).value_or(nullptr);
	}
	return ptr;
}

bool RtcpChainableHandler::send(message_ptr msg) {
	try {
		outgoingCallback(std::move(msg));
		return true;
	} catch (const std::exception &e) {
		LOG_DEBUG << "Send in RTCP chain handler failed: " << e.what();
	}
	return false;
}

void RtcpChainableHandler::addToChain(std::shared_ptr<MessageHandlerElement> chainable) {
	assert(leaf);
	leaf = leaf->chainWith(chainable);
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
