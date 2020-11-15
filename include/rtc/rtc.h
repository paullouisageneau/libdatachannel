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

#ifdef _WIN32
#define RTC_EXPORT __declspec(dllexport)
#ifdef CAPI_STDCALL
#define RTC_API __stdcall
#else
#define RTC_API
#endif
#else // not WIN32
#define RTC_EXPORT
#define RTC_API
#endif

#ifndef RTC_ENABLE_WEBSOCKET
#define RTC_ENABLE_WEBSOCKET 1
#endif

#include <stdbool.h>
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

typedef enum {
	RTC_SIGNALING_STABLE = 0,
	RTC_SIGNALING_HAVE_LOCAL_OFFER = 1,
	RTC_SIGNALING_HAVE_REMOTE_OFFER = 2,
	RTC_SIGNALING_HAVE_LOCAL_PRANSWER = 3,
	RTC_SIGNALING_HAVE_REMOTE_PRANSWER = 4,
} rtcSignalingState;

typedef enum { // Don't change, it must match plog severity
	RTC_LOG_NONE = 0,
	RTC_LOG_FATAL = 1,
	RTC_LOG_ERROR = 2,
	RTC_LOG_WARNING = 3,
	RTC_LOG_INFO = 4,
	RTC_LOG_DEBUG = 5,
	RTC_LOG_VERBOSE = 6
} rtcLogLevel;

#define RTC_ERR_SUCCESS 0
#define RTC_ERR_INVALID -1   // invalid argument
#define RTC_ERR_FAILURE -2   // runtime error
#define RTC_ERR_NOT_AVAIL -3 // element not available
#define RTC_ERR_TOO_SMALL -4 // buffer too small

typedef struct {
	const char **iceServers;
	int iceServersCount;
	uint16_t portRangeBegin;
	uint16_t portRangeEnd;
} rtcConfiguration;

typedef struct {
	bool unordered;
	bool unreliable;
	unsigned int maxPacketLifeTime; // ignored if reliable
	unsigned int maxRetransmits;    // ignored if reliable
} rtcReliability;

typedef struct {
	rtcReliability reliability;
	const char *protocol; // empty string if NULL
	bool negotiated;
	bool manualStream;
	uint16_t stream; // numeric ID 0-65534, ignored if manualStream is false
} rtcDataChannelInit;

typedef void(RTC_API *rtcLogCallbackFunc)(rtcLogLevel level, const char *message);
typedef void(RTC_API *rtcDescriptionCallbackFunc)(int pc, const char *sdp, const char *type,
                                                  void *ptr);
typedef void(RTC_API *rtcCandidateCallbackFunc)(int pc, const char *cand, const char *mid,
                                                void *ptr);
typedef void(RTC_API *rtcStateChangeCallbackFunc)(int pc, rtcState state, void *ptr);
typedef void(RTC_API *rtcGatheringStateCallbackFunc)(int pc, rtcGatheringState state, void *ptr);
typedef void(RTC_API *rtcSignalingStateCallbackFunc)(int pc, rtcSignalingState state, void *ptr);
typedef void(RTC_API *rtcDataChannelCallbackFunc)(int pc, int dc, void *ptr);
typedef void(RTC_API *rtcTrackCallbackFunc)(int pc, int tr, void *ptr);
typedef void(RTC_API *rtcOpenCallbackFunc)(int id, void *ptr);
typedef void(RTC_API *rtcClosedCallbackFunc)(int id, void *ptr);
typedef void(RTC_API *rtcErrorCallbackFunc)(int id, const char *error, void *ptr);
typedef void(RTC_API *rtcMessageCallbackFunc)(int id, const char *message, int size, void *ptr);
typedef void(RTC_API *rtcBufferedAmountLowCallbackFunc)(int id, void *ptr);
typedef void(RTC_API *rtcAvailableCallbackFunc)(int id, void *ptr);

// Log
// NULL cb on the first call will log to stdout
RTC_EXPORT void rtcInitLogger(rtcLogLevel level, rtcLogCallbackFunc cb);

// User pointer
RTC_EXPORT void rtcSetUserPointer(int id, void *ptr);

// PeerConnection
RTC_EXPORT int rtcCreatePeerConnection(const rtcConfiguration *config); // returns pc id
RTC_EXPORT int rtcDeletePeerConnection(int pc);

RTC_EXPORT int rtcSetLocalDescriptionCallback(int pc, rtcDescriptionCallbackFunc cb);
RTC_EXPORT int rtcSetLocalCandidateCallback(int pc, rtcCandidateCallbackFunc cb);
RTC_EXPORT int rtcSetStateChangeCallback(int pc, rtcStateChangeCallbackFunc cb);
RTC_EXPORT int rtcSetGatheringStateChangeCallback(int pc, rtcGatheringStateCallbackFunc cb);
RTC_EXPORT int rtcSetSignalingStateChangeCallback(int pc, rtcSignalingStateCallbackFunc cb);

RTC_EXPORT int rtcSetLocalDescription(int pc, const char *type);
RTC_EXPORT int rtcSetRemoteDescription(int pc, const char *sdp, const char *type);
RTC_EXPORT int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid);

RTC_EXPORT int rtcGetLocalDescription(int pc, char *buffer, int size);
RTC_EXPORT int rtcGetRemoteDescription(int pc, char *buffer, int size);

RTC_EXPORT int rtcGetLocalAddress(int pc, char *buffer, int size);
RTC_EXPORT int rtcGetRemoteAddress(int pc, char *buffer, int size);

RTC_EXPORT int rtcGetSelectedCandidatePair(int pc, char *local, int localSize, char *remote,
                                           int remoteSize);

// DataChannel
RTC_EXPORT int rtcSetDataChannelCallback(int pc, rtcDataChannelCallbackFunc cb);
RTC_EXPORT int rtcAddDataChannel(int pc, const char *label); // returns dc id
RTC_EXPORT int rtcAddDataChannelEx(int pc, const char *label,
                                   const rtcDataChannelInit *init); // returns dc id
// Equivalent to calling rtcAddDataChannel() and rtcSetLocalDescription()
RTC_EXPORT int rtcCreateDataChannel(int pc, const char *label); // returns dc id
RTC_EXPORT int rtcCreateDataChannelEx(int pc, const char *label,
                                      const rtcDataChannelInit *init); // returns dc id
RTC_EXPORT int rtcDeleteDataChannel(int dc);

RTC_EXPORT int rtcGetDataChannelStream(int dc);
RTC_EXPORT int rtcGetDataChannelLabel(int dc, char *buffer, int size);
RTC_EXPORT int rtcGetDataChannelProtocol(int dc, char *buffer, int size);
RTC_EXPORT int rtcGetDataChannelReliability(int dc, rtcReliability *reliability);

// Track
RTC_EXPORT int rtcSetTrackCallback(int pc, rtcTrackCallbackFunc cb);
RTC_EXPORT int rtcAddTrack(int pc, const char *mediaDescriptionSdp); // returns tr id
RTC_EXPORT int rtcDeleteTrack(int tr);

RTC_EXPORT int rtcGetTrackDescription(int tr, char *buffer, int size);

// WebSocket
#if RTC_ENABLE_WEBSOCKET
typedef struct {
	bool disableTlsVerification; // if true, don't verify the TLS certificate
} rtcWsConfiguration;

RTC_EXPORT int rtcCreateWebSocket(const char *url); // returns ws id
RTC_EXPORT int rtcCreateWebSocketEx(const char *url, const rtcWsConfiguration *config);
RTC_EXPORT int rtcDeleteWebsocket(int ws);
#endif

// DataChannel, Track, and WebSocket common API
RTC_EXPORT int rtcSetOpenCallback(int id, rtcOpenCallbackFunc cb);
RTC_EXPORT int rtcSetClosedCallback(int id, rtcClosedCallbackFunc cb);
RTC_EXPORT int rtcSetErrorCallback(int id, rtcErrorCallbackFunc cb);
RTC_EXPORT int rtcSetMessageCallback(int id, rtcMessageCallbackFunc cb);
RTC_EXPORT int rtcSendMessage(int id, const char *data, int size);

RTC_EXPORT int rtcGetBufferedAmount(int id); // total size buffered to send
RTC_EXPORT int rtcSetBufferedAmountLowThreshold(int id, int amount);
RTC_EXPORT int rtcSetBufferedAmountLowCallback(int id, rtcBufferedAmountLowCallbackFunc cb);

// DataChannel, Track, and WebSocket common extended API
RTC_EXPORT int rtcGetAvailableAmount(int id); // total size available to receive
RTC_EXPORT int rtcSetAvailableCallback(int id, rtcAvailableCallbackFunc cb);
RTC_EXPORT int rtcReceiveMessage(int id, char *buffer, int *size);

// Optional preload and cleanup
RTC_EXPORT void rtcPreload(void);
RTC_EXPORT void rtcCleanup(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
