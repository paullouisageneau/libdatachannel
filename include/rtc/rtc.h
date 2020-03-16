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

#ifndef RTC_C_API
#define RTC_C_API

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// libdatachannel C API

typedef enum {
	RTC_NEW = 0,
	RTC_CONNECTING = 1,
	RTC_CONNECTED = 2,
	RTC_DISCONNECTED = 3,
	RTC_FAILED = 4,
	RTC_CLOSED = 5
} rtcState;

typedef enum {
	RTC_GATHERING_NEW = 0,
	RTC_GATHERING_INPROGRESS = 1,
	RTC_GATHERING_COMPLETE = 2
} rtcGatheringState;

typedef enum { // Don't change, it must match plog severity
	RTC_LOG_NONE = 0,
	RTC_LOG_FATAL = 1,
	RTC_LOG_ERROR = 2,
	RTC_LOG_WARNING = 3,
	RTC_LOG_INFO = 4,
	RTC_LOG_DEBUG = 5,
	RTC_LOG_VERBOSE = 6
} rtcLogLevel;

typedef struct {
	const char **iceServers;
	int iceServersCount;
	uint16_t portRangeBegin;
	uint16_t portRangeEnd;
} rtcConfiguration;

typedef void (*dataChannelCallbackFunc)(int dc, void *ptr);
typedef void (*descriptionCallbackFunc)(const char *sdp, const char *type, void *ptr);
typedef void (*candidateCallbackFunc)(const char *cand, const char *mid, void *ptr);
typedef void (*stateChangeCallbackFunc)(rtcState state, void *ptr);
typedef void (*gatheringStateCallbackFunc)(rtcGatheringState state, void *ptr);
typedef void (*openCallbackFunc)(void *ptr);
typedef void (*closedCallbackFunc)(void *ptr);
typedef void (*errorCallbackFunc)(const char *error, void *ptr);
typedef void (*messageCallbackFunc)(const char *message, int size, void *ptr);
typedef void (*bufferedAmountLowCallbackFunc)(void *ptr);
typedef void (*availableCallbackFunc)(void *ptr);

// Log
void rtcInitLogger(rtcLogLevel level);

// User pointer
void rtcSetUserPointer(int id, void *ptr);

// PeerConnection
int rtcCreatePeerConnection(const rtcConfiguration *config); // returns pc id
int rtcDeletePeerConnection(int pc);

int rtcSetDataChannelCallback(int pc, dataChannelCallbackFunc cb);
int rtcSetLocalDescriptionCallback(int pc, descriptionCallbackFunc cb);
int rtcSetLocalCandidateCallback(int pc, candidateCallbackFunc cb);
int rtcSetStateChangeCallback(int pc, stateChangeCallbackFunc cb);
int rtcSetGatheringStateChangeCallback(int pc, gatheringStateCallbackFunc cb);

int rtcSetRemoteDescription(int pc, const char *sdp, const char *type);
int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid);

int rtcGetLocalAddress(int pc, char *buffer, int size);
int rtcGetRemoteAddress(int pc, char *buffer, int size);

// DataChannel
int rtcCreateDataChannel(int pc, const char *label); // returns dc id
int rtcDeleteDataChannel(int dc);

int rtcGetDataChannelLabel(int dc, char *buffer, int size);

// WebSocket
#if ENABLE_WEBSOCKET
int rtcCreateWebSocket(const char *url); // returns ws id
int rtcDeleteWebsocket(int ws);
#endif

// DataChannel and WebSocket common API
int rtcSetOpenCallback(int id, openCallbackFunc cb);
int rtcSetClosedCallback(int id, closedCallbackFunc cb);
int rtcSetErrorCallback(int id, errorCallbackFunc cb);
int rtcSetMessageCallback(int id, messageCallbackFunc cb);
int rtcSendMessage(int id, const char *data, int size);

int rtcGetBufferedAmount(int id); // total size buffered to send
int rtcSetBufferedAmountLowThreshold(int id, int amount);
int rtcSetBufferedAmountLowCallback(int id, bufferedAmountLowCallbackFunc cb);

// DataChannel and WebSocket common extended API
int rtcGetAvailableAmount(int id); // total size available to receive
int rtcSetAvailableCallback(int id, availableCallbackFunc cb);
int rtcReceiveMessage(int id, char *buffer, int *size);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
