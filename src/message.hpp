/**
 * Copyright (c) 2019 Paul-Louis Ageneau
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

#ifndef RTC_MESSAGE_H
#define RTC_MESSAGE_H

#include "include.hpp"
#include "reliability.hpp"

#include <functional>
#include <memory>

namespace rtc {

struct Message : binary {
	enum Type { Binary, String, Control };

	template <typename Iterator>
	Message(Iterator begin_, Iterator end_, Type type_ = Binary, unsigned int stream_ = 0,
	        std::shared_ptr<Reliability> reliability_ = nullptr)
	    : binary(begin_, end_), type(type_), stream(stream_), reliability(reliability_) {}

	Type type;
	unsigned int stream;
	std::shared_ptr<Reliability> reliability;
};

using message_ptr = std::shared_ptr<const Message>;
using message_callback = std::function<void(message_ptr message)>;

template <typename Iterator>
message_ptr make_message(Iterator begin, Iterator end, Message::Type type = Message::Binary,
                         unsigned int stream = 0,
                         std::shared_ptr<Reliability> reliability = nullptr) {
	return std::make_shared<Message>(begin, end, type, stream, reliability);
}

} // namespace rtc

#endif
