/**
 * Copyright (c) 2020 Paul-Louis Ageneau
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

#include <rtc/rtc.h>

#include <cstdbool>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h> // for sleep

using namespace std;

typedef struct {
	rtcState state;
	rtcGatheringState gatheringState;
	int pc;
	int dc;
	bool connected;
} Peer;

Peer *peer1 = NULL;
Peer *peer2 = NULL;

static void descriptionCallback(const char *sdp, const char *type, void *ptr) {
	Peer *peer = (Peer *)ptr;
	printf("Description %d:\n%s\n", peer == peer1 ? 1 : 2, sdp);
	Peer *other = peer == peer1 ? peer2 : peer1;
	rtcSetRemoteDescription(other->pc, sdp, type);
}

static void candidateCallback(const char *cand, const char *mid, void *ptr) {
	Peer *peer = (Peer *)ptr;
	printf("Candidate %d: %s\n", peer == peer1 ? 1 : 2, cand);
	Peer *other = peer == peer1 ? peer2 : peer1;
	rtcAddRemoteCandidate(other->pc, cand, mid);
}

static void stateChangeCallback(rtcState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->state = state;
	printf("State %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void gatheringStateCallback(rtcGatheringState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->gatheringState = state;
	printf("Gathering state %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void openCallback(void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = true;
	printf("DataChannel %d: Open\n", peer == peer1 ? 1 : 2);

	const char *message = peer == peer1 ? "Hello from 1" : "Hello from 2";
	rtcSendMessage(peer->dc, message, -1); // negative size indicates a null-terminated string
}

static void closedCallback(void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = false;
}

static void messageCallback(const char *message, int size, void *ptr) {
	Peer *peer = (Peer *)ptr;
	if (size < 0) { // negative size indicates a null-terminated string
		printf("Message %d: %s\n", peer == peer1 ? 1 : 2, message);
	} else {
		printf("Message %d: [binary of size %d]\n", peer == peer1 ? 1 : 2, size);
	}
}

static void dataChannelCallback(int dc, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->dc = dc;
	peer->connected = true;
	rtcSetClosedCallback(dc, closedCallback);
	rtcSetMessageCallback(dc, messageCallback);

	char buffer[256];
	if (rtcGetDataChannelLabel(dc, buffer, 256) >= 0)
		printf("DataChannel %d: Received with label \"%s\"\n", peer == peer1 ? 1 : 2, buffer);

	const char *message = peer == peer1 ? "Hello from 1" : "Hello from 2";
	rtcSendMessage(peer->dc, message, -1); // negative size indicates a null-terminated string
}

static Peer *createPeer(const rtcConfiguration *config) {
	Peer *peer = (Peer *)malloc(sizeof(Peer));
	if (!peer)
		return nullptr;
	memset(peer, 0, sizeof(Peer));

	// Create peer connection
	peer->pc = rtcCreatePeerConnection(config);
	rtcSetUserPointer(peer->pc, peer);
	rtcSetDataChannelCallback(peer->pc, dataChannelCallback);
	rtcSetLocalDescriptionCallback(peer->pc, descriptionCallback);
	rtcSetLocalCandidateCallback(peer->pc, candidateCallback);
	rtcSetStateChangeCallback(peer->pc, stateChangeCallback);
	rtcSetGatheringStateChangeCallback(peer->pc, gatheringStateCallback);

	return peer;
}

static void deletePeer(Peer *peer) {
	if (peer) {
		if (peer->dc)
			rtcDeleteDataChannel(peer->dc);
		if (peer->pc)
			rtcDeletePeerConnection(peer->pc);
		free(peer);
	}
}

int test_capi_main() {
	int attempts;

	rtcInitLogger(RTC_LOG_DEBUG);

	rtcConfiguration config;
	memset(&config, 0, sizeof(config));
	// const char *iceServers[1] = {"stun:stun.l.google.com:19302"};
	// config.iceServers = iceServers;
	// config.iceServersCount = 1;

	// Create peer 1
	peer1 = createPeer(&config);
	if (!peer1)
		goto error;

	// Create peer 2
	peer2 = createPeer(&config);
	if (!peer2)
		goto error;

	// Peer 1: Create data channel
	peer1->dc = rtcCreateDataChannel(peer1->pc, "test");
	rtcSetOpenCallback(peer1->dc, openCallback);
	rtcSetClosedCallback(peer1->dc, closedCallback);
	rtcSetMessageCallback(peer1->dc, messageCallback);

	attempts = 10;
	while (!peer2->connected && attempts--)
		sleep(1);

	if (peer1->state != RTC_CONNECTED || peer2->state != RTC_CONNECTED) {
		fprintf(stderr, "PeerConnection is not connected\n");
		goto error;
	}

	if (!peer1->connected || !peer2->connected) {
		fprintf(stderr, "DataChannel is not connected\n");
		goto error;
	}

	char buffer[256];
	if (rtcGetLocalAddress(peer1->pc, buffer, 256) >= 0)
		printf("Local address 1:  %s\n", buffer);
	if (rtcGetRemoteAddress(peer1->pc, buffer, 256) >= 0)
		printf("Remote address 1: %s\n", buffer);
	if (rtcGetLocalAddress(peer2->pc, buffer, 256) >= 0)
		printf("Local address 2:  %s\n", buffer);
	if (rtcGetRemoteAddress(peer2->pc, buffer, 256) >= 0)
		printf("Remote address 2: %s\n", buffer);

	deletePeer(peer1);
	sleep(1);
	deletePeer(peer2);

	printf("Success\n");
	return 0;

error:
	deletePeer(peer1);
	deletePeer(peer2);
	return -1;
}

#include <stdexcept>

void test_capi() {
	if (test_capi_main())
		throw std::runtime_error("Connection failed");
}
