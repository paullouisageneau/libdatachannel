/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "asio.hpp"

namespace rtc::impl {

Asio &Asio::Instance() {
	static Asio *instance = new Asio;
	return *instance;
}

void Asio::init(AsioSettings const &s) { mSettings = s; }

void Asio::start() { mSettings.startCallback(); }

void Asio::stop() { mSettings.stopCallback(); }

} // namespace rtc::impl
