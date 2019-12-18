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

// libdatachannel rtc C API

typedef enum {
	RTC_NEW = 0,
	RTC_CONNECTING = 1,
	RTC_CONNECTED = 2,
	RTC_DISCONNECTED = 3,
	RTC_FAILED = 4,
	RTC_CLOSED = 5,
	RTC_DESTROYING = 6 // internal
} rtc_state_t;

typedef enum {
	RTC_GATHERING_NEW = 0,
	RTC_GATHERING_INPROGRESS = 1,
	RTC_GATHERING_COMPLETE = 2
} rtc_gathering_state_t;

typedef enum { none, fatal, error, warning, info, debug, verbose } LogLevel;

void rtcInitLogger(LogLevel logLevel, plog::IAppender *appender = NULL);
int rtcCreatePeerConnection(const char **iceServers, int iceServersCount);
void rtcDeletePeerConnection(int pc);
int rtcCreateDataChannel(int pc, const char *label);
void rtcDeleteDataChannel(int dc);
void rtcSetDataChannelCallback(int pc, void (*dataChannelCallback)(int, void *));
void rtcSetLocalDescriptionCallback(int pc, void (*descriptionCallback)(const char *, const char *,
                                                                        void *));
void rtcSetLocalCandidateCallback(int pc,
                                  void (*candidateCallback)(const char *, const char *, void *));
void rtcSetStateChangeCallback(int pc, void (*stateCallback)(rtc_state_t state, void *));
void rtcSetGatheringStateChangeCallback(int pc,
                                        void (*gatheringStateCallback)(rtc_gathering_state_t state,
                                                                       void *));
void rtcSetRemoteDescription(int pc, const char *sdp, const char *type);
void rtcAddRemoteCandidate(int pc, const char *candidate, const char *mid);
int rtcGetDataChannelLabel(int dc, char *data, int size);
void rtcSetOpenCallback(int dc, void (*openCallback)(void *));
void rtcSetErrorCallback(int dc, void (*errorCallback)(const char *, void *));
void rtcSetMessageCallback(int dc, void (*messageCallback)(const char *, int, void *));
int rtcSendMessage(int dc, const char *data, int size);
void rtcSetUserPointer(int i, void *ptr);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
