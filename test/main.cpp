/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <chrono>
#include <iostream>
#include <thread>

#include "test.hpp"
#include <rtc/rtc.hpp>

using namespace std;
using namespace chrono_literals;

using chrono::duration_cast;
using chrono::milliseconds;
using chrono::seconds;
using chrono::steady_clock;

TestResult test_connectivity();
TestResult test_connectivity_fail_on_wrong_fingerprint();
TestResult test_pem();
TestResult test_negotiated();
TestResult test_reliability();
TestResult test_simulcast_sdp_generation();
TestResult test_simulcast_sdp_parsing();
TestResult test_turn_connectivity();
TestResult test_track();
TestResult test_video_layers_allocation();
TestResult test_fir_sdp();
TestResult test_fir_offer_yes_answer_yes();
TestResult test_rtx_attribute();
TestResult test_rtx_description_addrtx();
TestResult test_rtx_description_addrtx_no_audio();
TestResult test_rtx_dropped_packet();
TestResult test_rtx_multi_codec();
TestResult test_rtcp_app_single_packet();
TestResult test_rtcp_app_compound_packet();
TestResult test_rtcp_app_empty_data();
TestResult test_rtcp_app_send();
TestResult test_rtcp_app_multiple_in_compound();
TestResult test_rtcp_app_integration();
TestResult test_capi_connectivity();
#if RTC_ENABLE_MEDIA
TestResult test_sframe_crypto();
TestResult test_sframe_packetizer();
TestResult test_sframe_key_provider();
TestResult test_sframe_video_answer_yes();
TestResult test_sframe_video_answer_no();
TestResult test_sframe_attribute_with_parameters();
TestResult test_sframe_description_has_sframe();
TestResult test_sframe_session_key_provider();
TestResult test_sframe_session_key_provider_without_offer();
TestResult test_sframe_session_provider_unconfigurable_m_line();
TestResult test_sframe_session_provider_on_offerer();
TestResult test_sframe_video_declined_keeps_depacketizer();
TestResult test_sframe_audio_answer_yes();
TestResult test_sframe_audio_answer_no();
TestResult test_sframe_audio_static_payload_type();
TestResult test_sframe_replaced_chain_declines();
TestResult test_sframe_preserves_existing_chain();
TestResult test_sframe_replaces_codec_depacketizer();
TestResult test_sframe_keeps_packetizer_on_sendrecv();
TestResult test_sframe_send_only_applies_sframe();
TestResult test_sframe_multi_track_answer_yes();
TestResult test_sframe_multi_track_directions();
TestResult test_sframe_mixed_offer_protected_plain_and_stopped();
TestResult test_sframe_multi_track_partial_answer();
TestResult test_sframe_multi_track_ratcheting();
TestResult test_sframe_deferred_chain_multi_codec();
TestResult test_sframe_shared_key_multi_track();
TestResult test_sframe_rtcp_passthrough();
TestResult test_sframe_rtcp_passthrough_unprotected();
TestResult test_sframe_send_without_packetizer_refused();
TestResult test_sframe_send_with_packetizer_allowed();
TestResult test_sframe_send_guard_exempts_rtcp();
TestResult test_sframe_track_explicit_clock_rate();
TestResult test_sframe_track_clock_rate_from_rtpmap();
TestResult test_sframe_null_key_provider_refused();
TestResult test_sframe_session_provider_before_track_callback();
TestResult test_sframe_session_wide_receive_chain_keeps_attribute();
TestResult test_sframe_no_receive_chain_anywhere_stops_mline();
TestResult test_sframe_session_provider_with_packetizer_installed();
#endif
TestResult test_capi_track();
TestResult test_websocket();
TestResult test_websocketserver();
TestResult test_capi_websocketserver();
size_t benchmark(chrono::milliseconds duration);

void test_benchmark() {
	size_t goodput = benchmark(10s);

	if (goodput == 0)
		throw runtime_error("No data received");

	const size_t threshold = 1000; // 1 MB/s;
	if (goodput < threshold)
		throw runtime_error("Goodput is too low");
}

TestResult test_cleanup() {
	try {
		// Every created object must have been destroyed, otherwise the wait will block
		if (rtc::Cleanup().wait_for(10s) == future_status::timeout)
			return TestResult(false, "timeout");
		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

TestResult test_capi_cleanup() {
	try {
		rtcCleanup();
		return TestResult(true);
	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

static const vector<Test> tests = {
    // C++ API tests
    Test("WebRTC connectivity", test_connectivity),
    Test("WebRTC broken fingerprint", test_connectivity_fail_on_wrong_fingerprint),
    Test("pem", test_pem),
    // TODO: Temporarily disabled as the Open Relay TURN server is unreliable
    // Test("WebRTC TURN connectivity", test_turn_connectivity),
    Test("WebRTC negotiated DataChannel", test_negotiated),
    Test("WebRTC reliability mode", test_reliability),
    Test("WebRTC simulcast SDP generation", test_simulcast_sdp_generation),
    Test("WebRTC simulcast SDP parsing", test_simulcast_sdp_parsing),
#if RTC_ENABLE_MEDIA
    Test("WebRTC track", test_track),
	Test("WebRTC video layers allocation", test_video_layers_allocation),
    Test("RTX Description::addRtx", test_rtx_description_addrtx),
    Test("RTX Description::addRtx audio=false", test_rtx_description_addrtx_no_audio),
    Test("RTX negotiation fallback", test_rtx_attribute),
    Test("RTX dropped packet recovery", test_rtx_dropped_packet),
    Test("RTX multi-codec PT mapping", test_rtx_multi_codec),
    Test("SFrame crypto", test_sframe_crypto),
    Test("SFrame packetizer round-trip", test_sframe_packetizer),
    Test("SFrame key provider and ratcheting", test_sframe_key_provider),
    Test("SFrame video offer yes / answer yes", test_sframe_video_answer_yes),
    Test("SFrame video offer yes / answer no", test_sframe_video_answer_no),
    Test("SFrame a=sframe with parameters", test_sframe_attribute_with_parameters),
    Test("SFrame session-level hasSFrame", test_sframe_description_has_sframe),
    Test("SFrame session key provider", test_sframe_session_key_provider),
    Test("SFrame session provider, m-line without a=sframe",
         test_sframe_session_key_provider_without_offer),
    Test("SFrame session provider, unconfigurable m-line",
         test_sframe_session_provider_unconfigurable_m_line),
    Test("SFrame session provider on offerer", test_sframe_session_provider_on_offerer),
    Test("SFrame video answer no, SFrame depacketizer kept",
         test_sframe_video_declined_keeps_depacketizer),
    Test("SFrame audio offer yes / answer yes", test_sframe_audio_answer_yes),
    Test("SFrame audio offer yes / answer no", test_sframe_audio_answer_no),
    Test("SFrame audio static payload type", test_sframe_audio_static_payload_type),
    Test("SFrame replaced chain declines", test_sframe_replaced_chain_declines),
    Test("SFrame preserves existing chain", test_sframe_preserves_existing_chain),
    Test("SFrame replaces codec depacketizer", test_sframe_replaces_codec_depacketizer),
    Test("SFrame keeps packetizer on sendrecv", test_sframe_keeps_packetizer_on_sendrecv),
    Test("SFrame send-only applies SFrame", test_sframe_send_only_applies_sframe),
    Test("SFrame multi-track answer yes", test_sframe_multi_track_answer_yes),
    Test("SFrame multi-track directions", test_sframe_multi_track_directions),
    Test("SFrame mixed offer: protected, plain and stopped m-lines",
         test_sframe_mixed_offer_protected_plain_and_stopped),
    Test("SFrame multi-track partial answer", test_sframe_multi_track_partial_answer),
    Test("SFrame multi-track ratcheting", test_sframe_multi_track_ratcheting),
    Test("SFrame deferred chain multi-codec", test_sframe_deferred_chain_multi_codec),
    Test("SFrame shared key multi-track", test_sframe_shared_key_multi_track),
    Test("SFrame RTCP passthrough", test_sframe_rtcp_passthrough),
    Test("SFrame RTCP passthrough on unprotected m-lines",
         test_sframe_rtcp_passthrough_unprotected),
    Test("SFrame send refused without packetizer", test_sframe_send_without_packetizer_refused),
    Test("SFrame send allowed with packetizer", test_sframe_send_with_packetizer_allowed),
    Test("SFrame send guard exempts RTCP", test_sframe_send_guard_exempts_rtcp),
    Test("SFrame track explicit clock rate", test_sframe_track_explicit_clock_rate),
    Test("SFrame track clock rate from rtpmap", test_sframe_track_clock_rate_from_rtpmap),
    Test("SFrame null key provider refused", test_sframe_null_key_provider_refused),
    Test("SFrame session provider applied before track callback",
         test_sframe_session_provider_before_track_callback),
    Test("SFrame session-wide receive chain keeps a=sframe",
         test_sframe_session_wide_receive_chain_keeps_attribute),
    Test("SFrame no receive chain anywhere stops the m-line",
         test_sframe_no_receive_chain_anywhere_stops_mline),
    Test("SFrame session provider with packetizer installed",
         test_sframe_session_provider_with_packetizer_installed),
    Test("FIR SDP parsing", test_fir_sdp),
    Test("FIR offer answer handling", test_fir_offer_yes_answer_yes),
    Test("RTCP APP single packet", test_rtcp_app_single_packet),
    Test("RTCP APP compound packet", test_rtcp_app_compound_packet),
    Test("RTCP APP empty data", test_rtcp_app_empty_data),
    Test("RTCP APP send", test_rtcp_app_send),
    Test("RTCP APP multiple in compound", test_rtcp_app_multiple_in_compound),
    Test("RTCP APP integration", test_rtcp_app_integration),
#endif
#if RTC_ENABLE_WEBSOCKET
    // TODO: Temporarily disabled as the echo service is unreliable
    // Test("WebSocket", test_websocket),
    Test("WebSocketServer", test_websocketserver),
#endif
    Test("Cleanup", test_cleanup),
    // C API tests
    Test("WebRTC C API connectivity", test_capi_connectivity),
#if RTC_ENABLE_MEDIA
    Test("WebRTC C API track", test_capi_track),
#endif
#if RTC_ENABLE_WEBSOCKET
    Test("WebSocketServer C API", test_capi_websocketserver),
#endif
    Test("C API cleanup", test_capi_cleanup),
};

int main(int argc, char **argv) {
	rtc::SetThreadPoolSize(4);

	int success_tests = 0;
	int failed_tests = 0;
	steady_clock::time_point startTime, endTime;

	startTime = steady_clock::now();

	for (auto test : tests) {
		auto res = test.run();
		if (res.success) {
			success_tests++;
		} else {
			failed_tests++;
		}
	}

	endTime = steady_clock::now();

	auto durationMs = duration_cast<milliseconds>(endTime - startTime);
	auto durationS = duration_cast<seconds>(endTime - startTime);
	cout << "Finished " << success_tests + failed_tests << " tests in " << durationS.count()
	     << "s (" << durationMs.count() << " ms). Succeeded: " << success_tests
	     << ". Failed: " << failed_tests << "." << endl;

	/*
	    // Benchmark
	    try {
	        cout << endl << "*** Running WebRTC benchmark..." << endl;
	        test_benchmark();
	        cout << "*** Finished WebRTC benchmark" << endl;
	    } catch (const exception &e) {
	        cerr << "WebRTC benchmark failed: " << e.what() << endl;
	        std::this_thread::sleep_for(2s);
	        return -1;
	    }
	*/
	return 0;
}
