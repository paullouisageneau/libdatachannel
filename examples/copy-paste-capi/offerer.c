/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 * Copyright (c) 2020 Stevedan Ogochukwu Omodolor
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <rtc/rtc.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "getline.h"
#include <windows.h>
static void sleep(unsigned int secs) { Sleep(secs * 1000); }
#else
#include <unistd.h> // for sleep
#endif

typedef struct {
	rtcState state;
	rtcGatheringState gatheringState;
	int pc;
	int dc;
	bool connected;
} Peer;

static void RTC_API descriptionCallback(int pc, const char *sdp, const char *type, void *ptr);
static void RTC_API candidateCallback(int pc, const char *cand, const char *mid, void *ptr);
static void RTC_API stateChangeCallback(int pc, rtcState state, void *ptr);
static void RTC_API gatheringStateCallback(int pc, rtcGatheringState state, void *ptr);
static void RTC_API openCallback(int id, void *ptr);
static void RTC_API closedCallback(int id, void *ptr);
static void RTC_API messageCallback(int id, const char *message, int size, void *ptr);
static void deletePeer(Peer *peer);

char *state_print(rtcState state);
char *rtcGatheringState_print(rtcGatheringState state);

int all_space(const char *str);

int main(int argc, char **argv) {
	rtcInitLogger(RTC_LOG_WARNING, NULL);

	// Create peer
	rtcConfiguration config;
	memset(&config, 0, sizeof(config));

	Peer *peer = (Peer *)malloc(sizeof(Peer));
	if (!peer) {
		fprintf(stderr, "Error allocating memory for peer\n");
		return -1;
	}
	memset(peer, 0, sizeof(Peer));

	printf("Peer created\n");

	// Create peer connection
	peer->pc = rtcCreatePeerConnection(&config);
	rtcSetUserPointer(peer->pc, peer);
	rtcSetLocalDescriptionCallback(peer->pc, descriptionCallback);
	rtcSetLocalCandidateCallback(peer->pc, candidateCallback);
	rtcSetStateChangeCallback(peer->pc, stateChangeCallback);
	rtcSetGatheringStateChangeCallback(peer->pc, gatheringStateCallback);

	// Since we are the offerer, we will create a datachannel
	peer->dc = rtcCreateDataChannel(peer->pc, "test");
	rtcSetOpenCallback(peer->dc, openCallback);
	rtcSetClosedCallback(peer->dc, closedCallback);
	rtcSetMessageCallback(peer->dc, messageCallback);

	sleep(1);

	bool exit = false;
	while (!exit) {

		printf("\n");
		printf("***********************************************************************************"
		       "****\n");
		printf("* 0: Exit /"
		       " 1: Enter remote description /"
		       " 2: Enter remote candidate /"
		       " 3: Send message /"
		       " 4: Print Connection Info *\n"
		       "[Command]: ");

		int command = -1;
		int c;

		if (!scanf("%d", &command)) {
			break;
		}

		while ((c = getchar()) != '\n' && c != EOF) {
		}
		fflush(stdin);

		switch (command) {
		case 0: {
			exit = true;
			break;
		}
		case 1: {
			// Parse Description
			printf("[Description]: ");

			char *line = NULL;
			size_t len = 0;
			size_t read = 0;
			char *sdp = (char *)malloc(sizeof(char));
			while ((read = getline(&line, &len, stdin)) != -1 && !all_space(line)) {
				sdp = (char *)realloc(sdp, (strlen(sdp) + 1) + strlen(line) + 1);
				strcat(sdp, line);
			}
			printf("%s\n", sdp);
			rtcSetRemoteDescription(peer->pc, sdp, "answer");
			free(sdp);
			free(line);
			break;
		}
		case 2: {
			// Parse Candidate
			printf("[Candidate]: ");
			char *candidate = NULL;
			size_t candidate_size = 0;
			if (getline(&candidate, &candidate_size, stdin)) {
				rtcAddRemoteCandidate(peer->pc, candidate, NULL);
				free(candidate);

			} else {
				printf("Error reading line\n");
				break;
			}

			break;
		}
		case 3: {
			// Send Message
			if (!peer->connected) {
				printf("** Channel is not Open **");
				break;
			}
			printf("[Message]: ");
			char *message = NULL;
			size_t message_size = 0;
			if (getline(&message, &message_size, stdin)) {
				rtcSendMessage(peer->dc, message, -1);
				free(message);
			} else {
				printf("Error reading line\n");
				break;
			}

			break;
		}
		case 4: {
			// Connection Info
			if (!peer->connected) {
				printf("** Channel is not Open **");
				break;
			}
			char buffer[256];
			if (rtcGetLocalAddress(peer->pc, buffer, 256) >= 0)
				printf("Local address 1:  %s\n", buffer);
			if (rtcGetRemoteAddress(peer->pc, buffer, 256) >= 0)
				printf("Remote address 1: %s\n", buffer);

			else
				printf("Could not get Candidate Pair Info\n");
			break;
		}
		default: {
			printf("** Invalid Command **");
			break;
		}
		}
	}

	deletePeer(peer);
	return 0;
}

static void RTC_API descriptionCallback(int pc, const char *sdp, const char *type, void *ptr) {
	printf("Description %s:\n%s\n", "offerer", sdp);
}

static void RTC_API candidateCallback(int pc, const char *cand, const char *mid, void *ptr) {
	printf("Candidate %s: %s\n", "offerer", cand);
}

static void RTC_API stateChangeCallback(int pc, rtcState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->state = state;
	printf("State %s: %s\n", "offerer", state_print(state));
}

static void RTC_API gatheringStateCallback(int pc, rtcGatheringState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->gatheringState = state;
	printf("Gathering state %s: %s\n", "offerer", rtcGatheringState_print(state));
}

static void RTC_API openCallback(int id, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = true;
	char buffer[256];
	if (rtcGetDataChannelLabel(peer->dc, buffer, 256) >= 0)
		printf("DataChannel %s: Received with label \"%s\"\n", "offerer", buffer);
}

static void RTC_API closedCallback(int id, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->connected = false;
}

static void RTC_API messageCallback(int id, const char *message, int size, void *ptr) {
	if (size < 0) { // negative size indicates a null-terminated string
		printf("Message %s: %s\n", "offerer", message);
	} else {
		printf("Message %s: [binary of size %d]\n", "offerer", size);
	}
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

char *state_print(rtcState state) {
	char *str = NULL;
	switch (state) {
	case RTC_NEW:
		str = "RTC_NEW";
		break;
	case RTC_CONNECTING:
		str = "RTC_CONNECTING";
		break;
	case RTC_CONNECTED:
		str = "RTC_CONNECTED";
		break;
	case RTC_DISCONNECTED:
		str = "RTC_DISCONNECTED";
		break;
	case RTC_FAILED:
		str = "RTC_FAILED";
		break;
	case RTC_CLOSED:
		str = "RTC_CLOSED";
		break;
	default:
		break;
	}

	return str;
}

char *rtcGatheringState_print(rtcGatheringState state) {
	char *str = NULL;
	switch (state) {
	case RTC_GATHERING_NEW:
		str = "RTC_GATHERING_NEW";
		break;
	case RTC_GATHERING_INPROGRESS:
		str = "RTC_GATHERING_INPROGRESS";
		break;
	case RTC_GATHERING_COMPLETE:
		str = "RTC_GATHERING_COMPLETE";
		break;
	default:
		break;
	}

	return str;
}

int all_space(const char *str) {
	while (*str) {
		if (!isspace(*str++)) {
			return 0;
		}
	}

	return 1;
}
