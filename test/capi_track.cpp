/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <rtc/rtc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
static void sleep(unsigned int secs) { Sleep(secs * 1000); }
#else
#include <unistd.h> // for sleep
#endif

typedef struct {
	rtcState state;
	rtcGatheringState gatheringState;
	int pc;
	int tr;
	bool connected;
} Peer;

static Peer *peer1 = NULL;
static Peer *peer2 = NULL;

static const char *mediaDescription = "video 9 UDP/TLS/RTP/SAVPF\r\n"
                                      "a=mid:video\r\n"
                                      "a=sendonly\r\n";

static void RTC_API descriptionCallback(int pc, const char *sdp, const char *type, void *ptr) {
	Peer *peer = (Peer *)ptr;
	printf("Description %d:\n%s\n", peer == peer1 ? 1 : 2, sdp);
	Peer *other = peer == peer1 ? peer2 : peer1;
	rtcSetRemoteDescription(other->pc, sdp, type);
}

static void RTC_API candidateCallback(int pc, const char *cand, const char *mid, void *ptr) {
	Peer *peer = (Peer *)ptr;
	printf("Candidate %d: %s\n", peer == peer1 ? 1 : 2, cand);
	Peer *other = peer == peer1 ? peer2 : peer1;
	rtcAddRemoteCandidate(other->pc, cand, mid);
}

static void RTC_API stateChangeCallback(int pc, rtcState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->state = state;
	printf("State %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void RTC_API gatheringStateCallback(int pc, rtcGatheringState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->gatheringState = state;
	printf("Gathering state %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void RTC_API openCallback(int id, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = true;
	printf("Track %d: Open\n", peer == peer1 ? 1 : 2);
}

static void RTC_API closedCallback(int id, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = false;
	printf("Track %d: Closed\n", peer == peer1 ? 1 : 2);
}

static void RTC_API trackCallback(int pc, int tr, void *ptr) {
	Peer *peer = (Peer *)ptr;

	char buffer[1024];
	if (rtcGetTrackDescription(tr, buffer, 1024) < 0) {
		fprintf(stderr, "rtcGetTrackDescription failed\n");
		return;
	}

	printf("Track %d: Received with media description: \n%s\n", peer == peer1 ? 1 : 2, buffer);

	char mid[256];
	if (rtcGetTrackMid(tr, mid, 256) < 0 || strcmp(mid, "video") != 0) {
		fprintf(stderr, "rtcGetTrackMid failed\n");
		return;
	}

	// Description is reversed here
	rtcDirection direction;
	if (rtcGetTrackDirection(tr, &direction) < 0 || direction != RTC_DIRECTION_RECVONLY) {
		fprintf(stderr, "rtcGetTrackDirection failed\n");
		return;
	}

	peer->tr = tr;
	rtcSetOpenCallback(tr, openCallback);
	rtcSetClosedCallback(tr, closedCallback);
}

static Peer *createPeer(const rtcConfiguration *config) {
	Peer *peer = (Peer *)malloc(sizeof(Peer));
	if (!peer)
		return nullptr;

	memset(peer, 0, sizeof(Peer));

	// Create peer connection
	peer->pc = rtcCreatePeerConnection(config);
	if (peer->pc < 0) {
		fprintf(stderr, "PeerConnection creation failed\n");
		free(peer);
		return nullptr;
	}

	rtcSetUserPointer(peer->pc, peer);
	rtcSetTrackCallback(peer->pc, trackCallback);
	rtcSetLocalDescriptionCallback(peer->pc, descriptionCallback);
	rtcSetLocalCandidateCallback(peer->pc, candidateCallback);
	rtcSetStateChangeCallback(peer->pc, stateChangeCallback);
	rtcSetGatheringStateChangeCallback(peer->pc, gatheringStateCallback);

	peer->tr = -1;
	return peer;
}

static void deletePeer(Peer *peer) {
	if (peer) {
		if (peer->tr >= 0)
			rtcDeleteTrack(peer->tr);

		if (peer->pc >= 0)
			rtcDeletePeerConnection(peer->pc);

		free(peer);
	}
}

int test_capi_track_main() {
	int attempts;

	rtcInitLogger(RTC_LOG_DEBUG, nullptr);

	// Create peer 1
	rtcConfiguration config1;
	memset(&config1, 0, sizeof(config1));
	// STUN server example
	// const char *iceServers[1] = {"stun:stun.l.google.com:19302"};
	// config1.iceServers = iceServers;
	// config1.iceServersCount = 1;

	peer1 = createPeer(&config1);
	if (!peer1)
		goto error;

	// Create peer 2
	rtcConfiguration config2;
	memset(&config2, 0, sizeof(config2));
	// STUN server example
	// config2.iceServers = iceServers;
	// config2.iceServersCount = 1;
	// Port range example
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;

	peer2 = createPeer(&config2);
	if (!peer2)
		goto error;

	// Peer 1: Create track
	peer1->tr = rtcAddTrack(peer1->pc, mediaDescription);
	rtcSetOpenCallback(peer1->tr, openCallback);
	rtcSetClosedCallback(peer1->tr, closedCallback);

	char mid[256];
	if (rtcGetTrackMid(peer1->tr, mid, 256) < 0 || strcmp(mid, "video") != 0) {
		fprintf(stderr, "rtcGetTrackMid failed\n");
		goto error;
	}

	rtcDirection direction;
	if (rtcGetTrackDirection(peer1->tr, &direction) < 0 || direction != RTC_DIRECTION_SENDONLY) {
		fprintf(stderr, "rtcGetTrackDirection failed\n");
		goto error;
	}

	// Initiate the handshake
	rtcSetLocalDescription(peer1->pc, NULL);

	attempts = 10;
	while ((!peer2->connected || !peer1->connected) && attempts--)
		sleep(1);

	if (peer1->state != RTC_CONNECTED || peer2->state != RTC_CONNECTED) {
		fprintf(stderr, "PeerConnection is not connected\n");
		goto error;
	}

	if (!peer1->connected || !peer2->connected) {
		fprintf(stderr, "Track is not connected\n");
		goto error;
	}

	deletePeer(peer1);
	sleep(1);
	deletePeer(peer2);
	sleep(1);

	printf("Success\n");
	return 0;

error:
	deletePeer(peer1);
	deletePeer(peer2);
	return -1;
}

#include <stdexcept>

void test_capi_track() {
	if (test_capi_track_main())
		throw std::runtime_error("Connection failed");
}
