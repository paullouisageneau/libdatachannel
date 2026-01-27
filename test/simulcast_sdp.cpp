#include "rtc/global.hpp"
#include "rtc/description.hpp"
#include "test.hpp"

using namespace rtc;
using namespace std;

TestResult test_simulcast_sdp() {
	InitLogger(LogLevel::Debug);

	// Without attributes
	{
		auto media0 = Description::Media("m=video 49170 UDP/TLS/RTP/SAVPF 96", "video0");

		media0.addRid("rid0");
		media0.addRid("rid1");
		media0.addRid("rid2");

		const auto sdp0 = media0.generateSdp("\n");

		if (sdp0.find("a=rid:rid0 send\n") == string::npos) {
			return {false, "cannot find rid0"};
		}
		if (sdp0.find("a=rid:rid1 send\n") == string::npos) {
			return {false, "cannot find rid1"};
		}
		if (sdp0.find("a=rid:rid2 send\n") == string::npos) {
			return {false, "cannot find rid2"};
		}

		if (sdp0.find("a=simulcast:send rid0;rid1;rid2\n") == string::npos) {
			return {false, "cannot find simulcast"};
		}
	}

	// With attributes using builder
	{
		auto media1 = Description::Media("m=video 49170 UDP/TLS/RTP/SAVPF 96", "video0");

		media1.addRid(Description::RidBuilder("rid0")
			.max_width(1920)
			.max_height(1080)
			.max_fps(60)
			.build());
		media1.addRid(Description::RidBuilder("rid1")
			.max_height(720)
			.max_fps(30)
			.max_br(1500000)
			.build());
		media1.addRid(Description::RidBuilder("rid2")
			.max_width(340)
			.max_width(350)
			.max_width(360)	// Last one wins
			.max_fps(15)
			.max_br(400000)
			.custom("foo", "bar")
			.build());
		media1.addRid(Description::RidBuilder("rid3")
			.build());

		const auto sdp1 = media1.generateSdp("\n");

		if (sdp1.find("a=rid:rid0 send max-width=1920;max-height=1080;max-fps=60\n") == string::npos) {
			return {false, "cannot find rid0"};
		}
		if (sdp1.find("a=rid:rid1 send max-height=720;max-fps=30;max-br=1500000\n") == string::npos) {
			return {false, "cannot find rid1"};
		}
		if (sdp1.find("a=rid:rid2 send max-width=360;max-fps=15;max-br=400000;foo=bar\n") == string::npos) {
			return {false, "cannot find rid2"};
		}
		if (sdp1.find("a=rid:rid3 send\n") == string::npos) {
			return {false, "cannot find rid3"};
		}

		if (sdp1.find("a=simulcast:send rid0;rid1;rid2;rid3\n") == string::npos) {
			return {false, "cannot find simulcast"};
		}
	}

	return {true};
}
