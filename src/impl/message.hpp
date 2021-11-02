/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
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

#ifndef RTC_IMPL_MESSAGE_H
#define RTC_IMPL_MESSAGE_H

#include "common.hpp"
#include "reliability.hpp"

#include <functional>

namespace rtc::impl {

struct Message : binary {
	enum Type { Binary, String, Control, Reset };

	Message(const Message &message) = default;
	Message(size_t size, Type type_ = Binary) : binary(size), type(type_) {}

	template <typename Iterator>
	Message(Iterator begin_, Iterator end_, Type type_ = Binary)
	    : binary(begin_, end_), type(type_) {}

	Message(binary &&data, Type type_ = Binary) : binary(std::move(data)), type(type_) {}

	Type type;
	unsigned int stream = 0; // Stream id (SCTP stream or SSRC)
	unsigned int dscp = 0;   // Differentiated Services Code Point
	shared_ptr<Reliability> reliability;
};

inline size_t message_size_func(const message_ptr &m) {
	return m->type == Message::Binary || m->type == Message::String ? m->size() : 0;
}

template <typename Iterator>
message_ptr make_message(Iterator begin, Iterator end, Message::Type type = Message::Binary,
                         unsigned int stream = 0, shared_ptr<Reliability> reliability = nullptr) {
	auto message = std::make_shared<Message>(begin, end, type);
	message->stream = stream;
	message->reliability = reliability;
	return message;
}

message_ptr make_message(size_t size, Message::Type type = Message::Binary,
                                        unsigned int stream = 0,
                                        shared_ptr<Reliability> reliability = nullptr);

message_ptr make_message(binary &&data, Message::Type type = Message::Binary,
                                        unsigned int stream = 0,
                                        shared_ptr<Reliability> reliability = nullptr);

message_ptr make_message(message_variant data);

message_variant to_variant(Message &&message);

} // namespace rtc::impl

#endif
