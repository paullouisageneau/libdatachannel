/**
 * Copyright (c) 2023 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "mediahandler.hpp"

#include "impl/internals.hpp"

namespace rtc {

MediaHandler::MediaHandler() {}

MediaHandler::~MediaHandler() {}

void MediaHandler::addToChain(shared_ptr<MediaHandler> handler) { last()->setNext(handler); }

void MediaHandler::setNext(shared_ptr<MediaHandler> handler) {
	return std::atomic_store(&mNext, handler);
}

shared_ptr<MediaHandler> MediaHandler::next() { return std::atomic_load(&mNext); }

shared_ptr<const MediaHandler> MediaHandler::next() const { return std::atomic_load(&mNext); }

shared_ptr<MediaHandler> MediaHandler::last() {
	if (auto handler = next())
		return handler->last();
	else
		return shared_from_this();
}

shared_ptr<const MediaHandler> MediaHandler::last() const {
	if (auto handler = next())
		return handler->last();
	else
		return shared_from_this();
}

bool MediaHandler::requestKeyframe(const message_callback &send) {
	// Default implementation is to call next handler
	if (auto handler = next())
		return handler->requestKeyframe(send);
	else
		return false;
}

bool MediaHandler::requestBitrate(unsigned int bitrate, const message_callback &send) {
	// Default implementation is to call next handler
	if (auto handler = next())
		return handler->requestBitrate(bitrate, send);
	else
		return false;
}

void MediaHandler::mediaChain(const Description::Media &desc) {
	media(desc);

	if (auto handler = next())
		handler->mediaChain(desc);
}

void MediaHandler::incomingChain(message_vector &messages, const message_callback &send) {
	if (auto handler = next())
		handler->incomingChain(messages, send);

	incoming(messages, send);
}

void MediaHandler::outgoingChain(message_vector &messages, const message_callback &send) {
	outgoing(messages, send);

	if (auto handler = next())
		return handler->outgoingChain(messages, send);
}

} // namespace rtc
