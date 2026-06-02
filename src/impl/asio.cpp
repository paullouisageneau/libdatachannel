#include "asio.hpp"

namespace rtc::impl {

Asio &Asio::Instance() {
	static Asio *instance = new Asio;
	return *instance;
}

void Asio::init(AsioSettings const &s) { mSettings = s; }

void Asio::start() { mSettings.startCallback(mSettings.userContext); }

void Asio::stop() { mSettings.stopCallback(mSettings.userContext); }

} // namespace rtc::impl
