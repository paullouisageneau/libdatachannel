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

#define BUFFER_SIZE 4096

typedef struct {
	rtcState state;
	rtcIceState iceState;
	rtcGatheringState gatheringState;
	rtcSignalingState signalingState;
	int pc;
	int dc;
	bool connected;
} Peer;

static Peer *peer1 = NULL;
static Peer *peer2 = NULL;

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

static void RTC_API iceStateChangeCallback(int pc, rtcIceState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->iceState = state;
	printf("ICE state %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void RTC_API gatheringStateCallback(int pc, rtcGatheringState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->gatheringState = state;
	printf("Gathering state %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void RTC_API signalingStateCallback(int pc, rtcSignalingState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->signalingState = state;
	printf("Signaling state %d: %d\n", peer == peer1 ? 1 : 2, (int)state);
}

static void RTC_API openCallback(int id, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = true;
	printf("DataChannel %d: Open\n", peer == peer1 ? 1 : 2);

	if (!rtcIsOpen(id)) {
		fprintf(stderr, "rtcIsOpen failed\n");
		return;
	}

	if (rtcIsClosed(id)) {
		fprintf(stderr, "rtcIsClosed failed\n");
		return;
	}

	const char *message = peer == peer1 ? "Hello from 1" : "Hello from 2";
	rtcSendMessage(peer->dc, message, -1); // negative size indicates a null-terminated string
}

static void RTC_API closedCallback(int id, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = false;
	printf("DataChannel %d: Closed\n", peer == peer1 ? 1 : 2);
}

static void RTC_API messageCallback(int id, const char *message, int size, void *ptr) {
	Peer *peer = (Peer *)ptr;
	if (size < 0) { // negative size indicates a null-terminated string
		printf("Message %d: %s\n", peer == peer1 ? 1 : 2, message);
	} else {
		printf("Message %d: [binary of size %d]\n", peer == peer1 ? 1 : 2, size);
	}
}

static void RTC_API dataChannelCallback(int pc, int dc, void *ptr) {
	Peer *peer = (Peer *)ptr;

	char label[256];
	if (rtcGetDataChannelLabel(dc, label, 256) < 0) {
		fprintf(stderr, "rtcGetDataChannelLabel failed\n");
		return;
	}

	char protocol[256];
	if (rtcGetDataChannelProtocol(dc, protocol, 256) < 0) {
		fprintf(stderr, "rtcGetDataChannelProtocol failed\n");
		return;
	}

	rtcReliability reliability;
	if (rtcGetDataChannelReliability(dc, &reliability) < 0) {
		fprintf(stderr, "rtcGetDataChannelReliability failed\n");
		return;
	}

	printf("DataChannel %d: Received with label \"%s\" and protocol \"%s\"\n",
	       peer == peer1 ? 1 : 2, label, protocol);

	if (strcmp(label, "test") != 0) {
		fprintf(stderr, "Wrong DataChannel label\n");
		return;
	}

	if (strcmp(protocol, "protocol") != 0) {
		fprintf(stderr, "Wrong DataChannel protocol\n");
		return;
	}

	if (reliability.unordered == false) {
		fprintf(stderr, "Wrong DataChannel reliability\n");
		return;
	}

	rtcSetOpenCallback(dc, openCallback);
	rtcSetClosedCallback(dc, closedCallback);
	rtcSetMessageCallback(dc, messageCallback);

	peer->dc = dc;
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
	rtcSetIceStateChangeCallback(peer->pc, iceStateChangeCallback);
	rtcSetGatheringStateChangeCallback(peer->pc, gatheringStateCallback);
	rtcSetSignalingStateChangeCallback(peer->pc, signalingStateCallback);

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

int test_capi_connectivity_main() {
	int attempts;
	char buffer[BUFFER_SIZE];
	char buffer2[BUFFER_SIZE];
	const char *test = "foo";
	const int testLen = 3;
	int size = 0;

	rtcInitLogger(RTC_LOG_DEBUG, nullptr);

	if (rtcIsOpen(666)) {
		fprintf(stderr, "rtcIsOpen for invalid channel id failed\n");
		return -1;
	}

	if (rtcIsClosed(666)) {
		fprintf(stderr, "rtcIsOpen for invalid channel id failed\n");
		return -1;
	}

	// STUN server example (not necessary to connect locally)
	const char *iceServers[1] = {"stun:stun.l.google.com:19302"};

	// Create peer 1
	rtcConfiguration config1;
	memset(&config1, 0, sizeof(config1));
	config1.iceServers = iceServers;
	config1.iceServersCount = 1;
	// Custom MTU example
	config1.mtu = 1500;

	peer1 = createPeer(&config1);
	if (!peer1)
		goto error;

	// Create peer 2
	rtcConfiguration config2;
	memset(&config2, 0, sizeof(config2));
	// STUN server example (not necessary to connect locally)
	// Please do not use outside of libdatachannel tests
	config2.iceServers = iceServers;
	config2.iceServersCount = 1;
	// Custom MTU example
	config2.mtu = 1500;
	// Port range example
	config2.portRangeBegin = 5000;
	config2.portRangeEnd = 6000;

	peer2 = createPeer(&config2);
	if (!peer2)
		goto error;

	// Peer 1: Create data channel
	rtcDataChannelInit init;
	memset(&init, 0, sizeof(init));
	init.protocol = "protocol";
	init.reliability.unordered = true;

	peer1->dc = rtcCreateDataChannelEx(peer1->pc, "test", &init);
	rtcSetOpenCallback(peer1->dc, openCallback);
	rtcSetClosedCallback(peer1->dc, closedCallback);
	rtcSetMessageCallback(peer1->dc, messageCallback);

	attempts = 10;
	while ((!peer2->connected || !peer1->connected) && attempts--)
		sleep(1);

	if (peer1->state != RTC_CONNECTED || peer2->state != RTC_CONNECTED) {
		fprintf(stderr, "PeerConnection is not connected\n");
		goto error;
	}

	if ((peer1->iceState != RTC_ICE_CONNECTED && peer1->iceState != RTC_ICE_COMPLETED) ||
	    (peer2->iceState != RTC_ICE_CONNECTED && peer2->iceState != RTC_ICE_COMPLETED)) {
		fprintf(stderr, "PeerConnection is not connected\n");
		goto error;
	}

	if (!peer1->connected || !peer2->connected) {
		fprintf(stderr, "DataChannel is not connected\n");
		goto error;
	}

	if (rtcGetLocalDescriptionType(peer1->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetLocalDescriptionType failed\n");
		goto error;
	}
	printf("Local description type 1: %s\n", buffer);

	if (rtcGetLocalDescription(peer1->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetLocalDescription failed\n");
		goto error;
	}
	printf("Local description 1: %s\n", buffer);

	if (rtcGetRemoteDescriptionType(peer1->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetRemoteDescriptionType failed\n");
		goto error;
	}
	printf("Remote description type 1: %s\n", buffer);

	if (rtcGetRemoteDescription(peer1->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetRemoteDescription failed\n");
		goto error;
	}
	printf("Remote description 1: %s\n", buffer);

	if (rtcGetLocalDescriptionType(peer2->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetLocalDescriptionType failed\n");
		goto error;
	}
	printf("Local description type 2: %s\n", buffer);

	if (rtcGetLocalDescription(peer2->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetLocalDescription failed\n");
		goto error;
	}
	printf("Local description 2: %s\n", buffer);

	if (rtcGetRemoteDescriptionType(peer2->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetRemoteDescriptionType failed\n");
		goto error;
	}
	printf("Remote description type 2: %s\n", buffer);

	if (rtcGetRemoteDescription(peer2->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetRemoteDescription failed\n");
		goto error;
	}
	printf("Remote description 2: %s\n", buffer);

	if (rtcGetLocalAddress(peer1->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetLocalAddress failed\n");
		goto error;
	}
	printf("Local address 1: %s\n", buffer);

	if (rtcGetRemoteAddress(peer1->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetRemoteAddress failed\n");
		goto error;
	}
	printf("Remote address 1: %s\n", buffer);

	if (rtcGetLocalAddress(peer2->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetLocalAddress failed\n");
		goto error;
	}
	printf("Local address 2: %s\n", buffer);

	if (rtcGetRemoteAddress(peer2->pc, buffer, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetRemoteAddress failed\n");
		goto error;
	}
	printf("Remote address 2: %s\n", buffer);

	if (rtcGetSelectedCandidatePair(peer1->pc, buffer, BUFFER_SIZE, buffer2, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetSelectedCandidatePair failed\n");
		goto error;
	}
	printf("Local candidate 1:  %s\n", buffer);
	printf("Remote candidate 1: %s\n", buffer2);

	if (rtcGetSelectedCandidatePair(peer2->pc, buffer, BUFFER_SIZE, buffer2, BUFFER_SIZE) < 0) {
		fprintf(stderr, "rtcGetSelectedCandidatePair failed\n");
		goto error;
	}
	printf("Local candidate 2:  %s\n", buffer);
	printf("Remote candidate 2: %s\n", buffer2);

	if (rtcGetMaxDataChannelStream(peer1->pc) <= 0 || rtcGetMaxDataChannelStream(peer2->pc) <= 0) {
		fprintf(stderr, "rtcGetMaxDataChannelStream failed\n");
		goto error;
	}

	rtcSetMessageCallback(peer2->dc, NULL);
	if (rtcSendMessage(peer1->dc, test, testLen) < 0) {
		fprintf(stderr, "rtcSendMessage failed\n");
		goto error;
	}
	sleep(1);
	size = 0;
	if (rtcReceiveMessage(peer2->dc, NULL, &size) < 0 || size != testLen) {
		fprintf(stderr, "rtcReceiveMessage failed to peek message size\n");
		goto error;
	}
	if (rtcReceiveMessage(peer2->dc, buffer, &size) < 0 || size != testLen) {
		fprintf(stderr, "rtcReceiveMessage failed to get the message\n");
		goto error;
	}

	rtcClose(peer1->dc); // optional

	rtcClosePeerConnection(peer1->pc); // optional

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

void test_capi_connectivity() {
	if (test_capi_connectivity_main())
		throw std::runtime_error("Connection failed");
}
