#include "rtc/global.hpp"
#include "rtc/description.hpp"
#include "test.hpp"

using namespace rtc;
using namespace std;

namespace {

bool check_attribute(const std::vector<Description::RidAttribute>& attrs, string_view key, string_view value) {
	return std::find_if(attrs.begin(), attrs.end(), [key, value](const Description::RidAttribute& attr) {
		return attr.name() == key && attr.value() == value;
	}) != attrs.end();
}

}

TestResult test_simulcast_sdp_parsing() {
	InitLogger(LogLevel::Debug);

	// Simple
	{
		Description::Video video0("video0");
		video0.parseSdpLine("a=rid:layer0 send");
		video0.parseSdpLine("a=rid:layer1 send");
		video0.parseSdpLine("a=rid:layer2 send");

		const auto rid_list = video0.rids();
		if (rid_list.size() != 3) {
			return {false, "invalid rid list size"};
		}

		const auto& rid0 = rid_list[0];
		if (rid0.rid() != "layer0") {
			return {false, "rid does not have layer0"};
		}
		if (!rid0.attributes().empty()) {
			return {false, "rid layer0 should not have attributes"};
		}

		const auto& rid1 = rid_list[1];
		if (rid1.rid() != "layer1") {
			return {false, "rid does not have layer1"};
		}
		if (!rid1.attributes().empty()) {
			return {false, "rid layer1 should not have attributes"};
		}

		const auto& rid2 = rid_list[2];
		if (rid2.rid() != "layer2") {
			return {false, "rid does not have layer2"};
		}
		if (!rid2.attributes().empty()) {
			return {false, "rid layer2 should not have attributes"};
		}
	}

	// With attributes
	{
		Description::Video video0("video0");
		video0.parseSdpLine("a=rid:layer0 send max-width=1920;max-height=1080;max-fps=60");
		video0.parseSdpLine("a=rid:layer1 send max-height=720;max-fps=20;max-fps=30;max-br=1500000;foo=bar");

		const auto rid_list = video0.rids();
		if (rid_list.size() != 2) {
			return {false, "invalid rid list size"};
		}

		const auto& rid0 = rid_list[0];
		if (rid0.rid() != "layer0") {
			return {false, "rid does not have layer0"};
		}
		const auto& attrs0 = rid0.attributes();
		if (attrs0.size() != 3 ||
			!check_attribute(attrs0, "max-width", "1920") ||
			!check_attribute(attrs0, "max-height", "1080") ||
			!check_attribute(attrs0, "max-fps", "60")) {
			return {false, "rid layer0 has invalid attributes"};
		}

		const auto& rid1 = rid_list[1];
		if (rid1.rid() != "layer1") {
			return {false, "rid does not have layer1"};
		}
		const auto& attrs1 = rid1.attributes();
		if (attrs1.size() != 4 ||
			!check_attribute(attrs1, "max-height", "720") ||
			!check_attribute(attrs1, "max-fps", "30") ||
			!check_attribute(attrs1, "max-br", "1500000") ||
			!check_attribute(attrs1, "foo", "bar")) {
			return {false, "rid layer1 has invalid attributes"};
		}
	}

	return {true};
}