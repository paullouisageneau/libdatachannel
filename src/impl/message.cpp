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

#include "message.hpp"

namespace rtc::impl {

message_ptr make_message(size_t size, Message::Type type, unsigned int stream,
                         shared_ptr<Reliability> reliability) {
	auto message = std::make_shared<Message>(size, type);
	message->stream = stream;
	message->reliability = reliability;
	return message;
}

message_ptr make_message(binary &&data, Message::Type type, unsigned int stream,
                         shared_ptr<Reliability> reliability) {
	auto message = std::make_shared<Message>(std::move(data), type);
	message->stream = stream;
	message->reliability = reliability;
	return message;
}

message_ptr make_message(message_variant data) {
	return std::visit( //
	    overloaded{
	        [&](binary data) { return make_message(std::move(data), Message::Binary); },
	        [&](string data) {
		        auto b = reinterpret_cast<const byte *>(data.data());
		        return make_message(b, b + data.size(), Message::String);
	        },
	    },
	    std::move(data));
}

message_variant to_variant(Message &&message) {
	switch (message.type) {
	case Message::String:
		return string(reinterpret_cast<const char *>(message.data()), message.size());
	default:
		return std::move(message);
	}
}

} // namespace rtc::impl
