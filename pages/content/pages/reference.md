Title: Reference
url: pages/reference.html

## libdatachannel - C API Documentation

The following details the C API of libdatachannel. The C API is available by including the `rtc/rtc.h` header.

### General considerations

Unless stated otherwise, functions return `RTC_ERR_SUCCESS`, defined as `0`, on success.

All functions can return the following negative error codes:

- `RTC_ERR_INVALID`: an invalid argument was provided
- `RTC_ERR_FAILURE`: a runtime error happened
- `RTC_ERR_NOT_AVAIL`: an element is not available at the moment
- `RTC_ERR_TOO_SMALL`: a user-provided buffer is too small

All functions taking pointers as arguments (excepted the opaque user pointer) need the memory to be accessible until the call returns, but it can be safely discarded afterwards.

### Common

#### rtcInitLogger

```
void rtcInitLogger(rtcLogLevel level, rtcLogCallbackFunc cb)
```

Arguments:

- `level`: the log level. It must be one of the following: `RTC_LOG_NONE`, `RTC_LOG_FATAL`, `RTC_LOG_ERROR`, `RTC_LOG_WARNING`, `RTC_LOG_INFO`, `RTC_LOG_DEBUG`, `RTC_LOG_VERBOSE`.
- `cb` (optional): the callback to pass the log lines to. If the callback is already set, it is replaced. If NULL after a callback is set, the callback is unset. If NULL on first call, the library will log to stdout instead.

`cb` must have the following signature:
`void myLogCallback(rtcLogLevel level, const char *message)`

Arguments:

- `level`: the log level for the current message. It will be one of the following: `RTC_LOG_FATAL`, `RTC_LOG_ERROR`, `RTC_LOG_WARNING`, `RTC_LOG_INFO`, `RTC_LOG_DEBUG`, `RTC_LOG_VERBOSE`.
- `message`: a null-terminated string containing the log message

#### rtcPreload

```
void rtcPreload(void)
```

An optional call to `rtcPreload` preloads the global resources used by the library. If it is not called, resources are lazy-loaded when they are required for the first time by a PeerConnection, which for instance prevents from properly timing connection establishment (as the first one will take way more time). The call blocks until preloading is finished. If resources are already loaded, the call has no effect.

#### rtcCleanup

```
void rtcCleanup(void)
```

An optional call to `rtcCleanup` unloads the global resources used by the library. The call will block until unloading is done. If Peer Connections, Data Channels, Tracks, or WebSockets created through this API still exist, they will be destroyed. If resources are already unloaded, the call has no effect.

Warning: This function requires all Peer Connections, Data Channels, Tracks, and WebSockets to be destroyed before returning, meaning all callbacks must return before this function returns. Therefore, it must never be called from a callback.

#### rtcSetUserPointer

```
void rtcSetUserPointer(int id, void *user_ptr)
```

Sets a opaque user pointer for a Peer Connection, Data Channel, Track, or WebSocket. The user pointer will be passed as last argument in each corresponding callback. It will never be accessed in any way. The initial user pointer of a Peer Connection or WebSocket is `NULL`, and the initial one of a Data Channel or Track is the one of the Peer Connection at the time of creation.

Arguments:

- `id`: the identifier of Peer Connection, Data Channel, Track, or WebSocket
- `user_ptr`: an opaque pointer whose meaning is up to the user

### PeerConnection

#### rtcCreatePeerConnection

```
int rtcCreatePeerConnection(const rtcConfiguration *config)

typedef struct {
	const char **iceServers;
	int iceServersCount;
	const char *proxyServer;
	const char *bindAddress;
	rtcCertificateType certificateType;
	rtcTransportPolicy iceTransportPolicy;
	bool enableIceTcp;
	bool enableIceUdpMux;
	bool disableAutoNegotiation;
	bool forceMediaTransport;
	uint16_t portRangeBegin;
	uint16_t portRangeEnd;
	int mtu;
	int maxMessageSize;
} rtcConfiguration;
```

Creates a Peer Connection.

Arguments:

- `config`: the configuration structure, containing:
  - `iceServers` (optional): an array of pointers on null-terminated ICE server URIs (NULL if unused)
  - `iceServersCount` (optional): number of URLs in the array pointed by `iceServers` (0 if unused)
  - `proxyServer` (optional): if non-NULL, specifies the proxy server URI to use for TURN relaying (ignored with libjuice as ICE backend)
  - `bindAddress` (optional): if non-NULL, bind only to the given local address (ignored with libnice as ICE backend)
  - `certificateType` (optional): certificate type, either `RTC_CERTIFICATE_ECDSA` or `RTC_CERTIFICATE_RSA` (0 or `RTC_CERTIFICATE_DEFAULT` if default)
  - `iceTransportPolicy` (optional): ICE transport policy, if set to `RTC_TRANSPORT_POLICY_RELAY`, the PeerConnection will emit only relayed candidates (0 or `RTC_TRANSPORT_POLICY_ALL` if default)
  - `enableIceTcp`: if true, generate TCP candidates for ICE (ignored with libjuice as ICE backend)
  - `enableIceUdpMux`: if true, connections are multiplexed on the same UDP port (should be combined with `portRangeBegin` and `portRangeEnd`, ignored with libnice as ICE backend)
  - `disableAutoNegotiation`: if true, the user is responsible for calling `rtcSetLocalDescription` after creating a Data Channel and after setting the remote description
  - `forceMediaTransport`: if true, the connection allocates the SRTP media transport even if no tracks are present (necessary to add tracks during later renegotiation)
  - `portRangeBegin` (optional): first port (included) of the allowed local port range (0 if unused)
  - `portRangeEnd` (optional): last port (included) of the allowed local port range (0 if unused)
  - `mtu` (optional): manually set the Maximum Transfer Unit (MTU) for the connection (0 if automatic)
  - `maxMessageSize` (optional): manually set the local maximum message size for Data Channels (0 if default)

Return value: the identifier of the new Peer Connection or a negative error code.

The Peer Connection must be deleted with `rtcDeletePeerConnection`.

Each entry in `iceServers` must match the format `[("stun"|"turn"|"turns") (":"|"://")][username ":" password "@"]hostname[":" port]["?transport=" ("udp"|"tcp"|"tls")]`. The default scheme is STUN, the default port is 3478 (5349 over TLS), and the default transport is UDP. For instance, a STUN server URI could be `mystunserver.org`, and a TURN server URI could be `turn:myuser:12345678@turnserver.org`. Note transports TCP and TLS are only available for a TURN server with libnice as ICE backend and govern only the TURN control connection, meaning relaying is always performed over UDP.

The `proxyServer` URI, if present, must match the format `[("http"|"socks5") (":"|"://")][username ":" password "@"]hostname["    :" port]`. The default scheme is HTTP, and the default port is 3128 for HTTP or 1080 for SOCKS5.

If the username or password of an URI contains reserved special characters, they must be percent-encoded. In particular, ":" must be encoded as "%3A" and "@" must by encoded as "%40".

#### rtcClosePeerConnection

```
int rtcClosePeerConnection(int pc)
```

Closes the Peer Connection.

Arguments:

- `pc`: the Peer Connection identifier

Return value: `RTC_ERR_SUCCESS` or a negative error code

#### rtcDeletePeerConnection

```
int rtcDeletePeerConnection(int pc)
```

Deletes the Peer Connection.

Arguments:

- `pc`: the Peer Connection identifier

Return value: `RTC_ERR_SUCCESS` or a negative error code

If it is not already closed, the Peer Connection is implicitly closed before being deleted. After this function has been called, `pc` must not be used in a function call anymore. This function will block until all scheduled callbacks of `pc` return (except the one this function might be called in) and no other callback will be called for `pc` after it returns.

#### rtcSetXCallback

These functions set, change, or unset (if `cb` is `NULL`) the different callbacks of a Peer Connection.

```
int rtcSetLocalDescriptionCallback(int pc, rtcDescriptionCallbackFunc cb)*
```

`cb` must have the following signature: `void myDescriptionCallback(int pc, const char *sdp, const char *type, void *user_ptr)`

```
int rtcSetLocalCandidateCallback(int pc, rtcCandidateCallbackFunc cb)
```

`cb` must have the following signature: `void myCandidateCallback(int pc, const char *cand, const char *mid, void *user_ptr)`

```
int rtcSetStateChangeCallback(int pc, rtcStateChangeCallbackFunc cb)
```

`cb` must have the following signature: `void myStateChangeCallback(int pc, rtcState state, void *user_ptr)`

`state` will be one of the following: `RTC_CONNECTING`, `RTC_CONNECTED`, `RTC_DISCONNECTED`, `RTC_FAILED`, or `RTC_CLOSED`.

```
int rtcSetGatheringStateChangeCallback(int pc, rtcGatheringStateCallbackFunc cb)
```

`void myGatheringStateCallback(int pc, rtcGatheringState state, void *user_ptr)`

`state` will be `RTC_GATHERING_INPROGRESS` or `RTC_GATHERING_COMPLETE`.

```
int rtcSetDataChannelCallback(int pc, rtcDataChannelCallbackFunc cb)
```

`cb` must have the following signature: `void myDataChannelCallback(int pc, int dc, void *user_ptr)`

```
int rtcSetTrackCallback(int pc, rtcTrackCallbackFunc cb)
```

`cb` must have the following signature: `void myTrackCallback(int pc, int tr, void *user_ptr)`

#### rtcSetLocalDescription

```
int rtcSetLocalDescription(int pc, const char *type)
```

Initiates the handshake process. Following this call, the local description callback will be called with the local description, which must be sent to the remote peer by the user's method of choice. Note this call is implicit after `rtcSetRemoteDescription` and `rtcCreateDataChannel` if `disableAutoNegotiation` was not set on Peer Connection creation.

Arguments:

- `pc`: the Peer Connection identifier
- `type` (optional): type of the description ("offer", "answer", "pranswer", or "rollback") or NULL for autodetection.

#### rtcSetRemoteDescription

```
int rtcSetRemoteDescription(int pc, const char *sdp, const char *type)
```

Sets the remote description received from the remote peer by the user's method of choice. The remote description may have candidates or not.

Arguments:

- `pc`: the Peer Connection identifier
- `type` (optional): type of the description ("offer", "answer", "pranswer", or "rollback") or NULL for autodetection.

If the remote description is an offer and `disableAutoNegotiation` was not set in `rtcConfiguration`, the library will automatically answer by calling `rtcSetLocalDescription` internally. Otherwise, the user must call it to answer the remote description.

#### rtcAddRemoteCandidate

```
int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid)
```

Adds a trickled remote candidate received from the remote peer by the user's method of choice.

Arguments:

- `pc`: the Peer Connection identifier
- `cand`: a null-terminated SDP string representing the candidate (with or without the `"a="` prefix)
- `mid` (optional): a null-terminated string representing the mid of the candidate in the remote SDP description or NULL for autodetection

The Peer Connection must have a remote description set.

Return value: `RTC_ERR_SUCCESS` or a negative error code

#### rtcGetLocalDescription

```
int rtcGetLocalDescription(int pc, char *buffer, int size)
```

Retrieves the current local description in SDP format.

Arguments:

- `pc`: the Peer Connection identifier
- `buffer`: a user-supplied buffer to store the description
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the description is not copied but the size is still returned.

#### rtcGetRemoteDescription

```
int rtcGetRemoteDescription(int pc, char *buffer, int size)
```

Retrieves the current remote description in SDP format.

Arguments:

- `pc`: the Peer Connection identifier
- `buffer`: a user-supplied buffer to store the description
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the description is not copied but the size is still returned.

#### rtcGetLocalDescriptionType

```
int rtcGetLocalDescriptionType(int pc, char *buffer, int size)
```

Retrieves the current local description type as string.

Arguments:

- `pc`: the Peer Connection identifier
- `buffer`: a user-supplied buffer to store the type
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the description is not copied but the size is still returned.

#### rtcGetRemoteDescription

```
int rtcGetRemoteDescriptionType(int pc, char *buffer, int size)
```

Retrieves the current remote description type as string.

Arguments:

- `pc`: the Peer Connection identifier
- `buffer`: a user-supplied buffer to store the type
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the description is not copied but the size is still returned.


#### rtcGetLocalAddress

```
int rtcGetLocalAddress(int pc, char *buffer, int size)
```

Retrieves the current local address, i.e. the network address of the currently selected local candidate. The address will have the format `"IP_ADDRESS:PORT"`, where `IP_ADDRESS` may be either IPv4 or IPv6. The call might fail if the PeerConnection is not in state `RTC_CONNECTED`, and the address might change after connection.

Arguments:

- `pc`: the Peer Connection identifier
- `buffer`: a user-supplied buffer to store the address
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the address is not copied but the size is still returned.

#### rtcGetRemoteAddress

```
int rtcGetRemoteAddress(int pc, char *buffer, int size)
```

Retrieves the current remote address, i.e. the network address of the currently selected remote candidate. The address will have the format `"IP_ADDRESS:PORT"`, where `IP_ADDRESS` may be either IPv4 or IPv6. The call may fail if the state is not `RTC_CONNECTED`, and the address might change after connection.

Arguments:

- `pc`: the Peer Connection identifier
- `buffer`: a user-supplied buffer to store the address
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the address is not copied but the size is still returned.

#### rtcGetSelectedCandidatePair

```
int rtcGetSelectedCandidatePair(int pc, char *local, int localSize, char *remote, int remoteSize)
```

Retrieves the currently selected candidate pair. The call may fail if the state is not `RTC_CONNECTED`, and the selected candidate pair might change after connection.

Arguments:

- `pc`: the Peer Connection identifier
- `local`: a user-supplied buffer to store the local candidate
- `localSize`: the size of `local`
- `remote`: a user-supplied buffer to store the remote candidate
- `remoteSize`: the size of `remote`

Return value: the maximun length of strings copied in buffers (including the terminating null character) or a negative error code

If `local`, `remote`, or both, are `NULL`, the corresponding candidate is not copied, but the maximum length is still returned.

#### rtcGetMaxDataChannelStream
```
int rtcGetMaxDataChannelStream(int pc);
```

Retrieves the maximum stream ID a Data Channel may use. It is useful to create user-negotiated Data Channels with `negotiated=true` and `manualStream=true`. The maximum is negotiated during connection, therefore the final value after connection might be lower than before connection if the remote maximum is lower.

Arguments:
- `pc`: the Peer Connection identifier

Return value: the maximum stream ID (`stream` for a Data Channel may be set from 0 to this value included) or a negative error code

#### rtcGetRemoteMaxMessageSize

```
int rtcGetRemoteMaxMessageSize(int pc)
```

Retrieves the maximum message size for data channels on the peer connection as negotiated with the remote peer.

Arguments:

- `pc`: the Peer Connection identifier

Return value: the maximum message size for data channels or a negative error code

### Channel (Common API for Data Channel, Track, and WebSocket)

The following common functions might be called with a generic channel identifier. It may be the identifier of either a Data Channel, a Track, or a WebSocket.

#### rtcSetXCallback

These functions set, change, or unset (if `cb` is `NULL`) the different callbacks of a channel identified by the argument `id`.

```
int rtcSetOpenCallback(int id, rtcOpenCallbackFunc cb)
```

`cb` must have the following signature: `void myOpenCallback(int id, void *user_ptr)`

It is called when the channel was previously connecting and is now open.

```
int rtcSetClosedCallback(int id, rtcClosedCallbackFunc cb)
```

`cb` must have the following signature: `void myClosedCallback(int id, void *user_ptr)`

It is called when the channel was previously open and is now closed.

```
int rtcSetErrorCallback(int id, rtcErrorCallbackFunc cb)
```

`cb` must have the following signature: `void myErrorCallback(int id, const char *error, void *user_ptr)`

It is called when the channel experience an error, either while connecting or open.

```
int rtcSetMessageCallback(int id, rtcMessageCallbackFunc cb)
```

`cb` must have the following signature: `void myMessageCallback(int id, const char *message, int size, void *user_ptr)`

It is called when the channel receives a message. While it is set, messages can't be received with `rtcReceiveMessage`.

Track: By default, the track receives data as RTP packets.

```
int rtcSetBufferedAmountLowCallback(int id, rtcBufferedAmountLowCallbackFunc cb)
```

`cb` must have the following signature: `void myBufferedAmountLowCallback(int id, void *user_ptr)`

It is called when the buffered amount was strictly higher than the threshold (see `rtcSetBufferedAmountLowThreshold`) and is now lower or equal than the threshold.

```
int rtcSetAvailableCallback(int id, rtcAvailableCallbackFunc cb)
```

`cb` must have the following signature: `void myAvailableCallback(int id, void *user_ptr)`

It is called when messages are now available to be received with `rtcReceiveMessage`.

#### rtcSendMessage

```
int rtcSendMessage(int id, const char *data, int size)
```

Sends a message in the channel.

Arguments:

- `id`: the channel identifier
- `data`: the message data
- `size`: if size >= 0, `data` is interpreted as a binary message of length `size`, otherwise it is interpreted as a null-terminated UTF-8 string.

Return value: `RTC_ERR_SUCCESS` or a negative error code

The message is sent immediately if possible, otherwise it is buffered to be sent later.

Data Channel and WebSocket: If the message may not be sent immediately due to flow control or congestion control, it is buffered until it can actually be sent. You can retrieve the current buffered data size with `rtcGetBufferedAmount`.

Track: By default, the track expects RTP packets. There is no flow or congestion control, packets are never buffered and `rtcGetBufferedAmount` always returns 0.

#### rtcClose

```
int rtcClose(int id)
```

Close the channel.

Arguments:

- `id`: the channel identifier

Return value: `RTC_ERR_SUCCESS` or a negative error code

WebSocket: Like with the JavaScript API, the state will first change to closing, then closed only after the connection has been actually closed.

#### rtcDelete

```
int rtcDelete(int id)
```

Deletes the channel.

Arguments:

- `id`: the channel identifier

Return value: `RTC_ERR_SUCCESS` or a negative error code

If it is not already closed, the channel is implicitly closed before being deleted. After this function has been called, `id` must not be used in a function call anymore. This function will block until all scheduled callbacks of `id` return (except the one this function might be called in) and no other callback will be called for `id` after it returns.

#### rtcIsOpen

```
bool rtcIsOpen(int id)
```

Arguments:

- `id`: the channel identifier

Return value: `true` if the channel exists and is open, `false` otherwise

#### rtcIsClosed

```
bool rtcIsClosed(int id)
```

Arguments:

- `id`: the channel identifier

Return value: `true` if the channel exists and is closed (not open and not connecting), `false` otherwise

#### rtcGetMaxMessageSize

```
int rtcGetMaxMessageSize(int id)
```

Retrieves the maximum message size for the channel.

Arguments:

- `id`: the channel identifier

Return value: the maximum message size or a negative error code

#### rtcGetBufferedAmount

```
int rtcGetBufferedAmount(int id)
```

Retrieves the current buffered amount, i.e. the total size of currently buffered messages waiting to be actually sent in the channel. This does not account for the data buffered at the transport level.

Arguments:

- `id`: the channel identifier

Return value: the buffered amount or a negative error code

#### rtcSetBufferedAmountLowThreshold

```
int rtcSetBufferedAmountLowThreshold(int id, int amount)
```

Changes the buffered amount threshold under which `BufferedAmountLowCallback` is called. The callback is called when the buffered amount was strictly superior and gets equal to or lower than the threshold when a message is sent. The initial threshold is 0, meaning the the callback is called each time the buffered amount goes back to zero after being non-zero.

Arguments:

- `id`: the channel identifier
- `amount`: the new buffer level threshold

Return value: `RTC_ERR_SUCCESS` or a negative error code

#### rtcReceiveMessage

```
int rtcReceiveMessage(int id, char *buffer, int *size)
```

Receives a pending message if possible. The function may only be called if `MessageCallback` is not set.

Arguments:

- `id`: the channel identifier
- `buffer`: a user-supplied buffer where to write the message data
- `size`: a pointer to a user-supplied int which must be initialized to the size of `buffer`. On success, the function will write the size of the message to it before returning (positive size if binary, negative size including terminating 0 if string).

Return value: `RTC_ERR_SUCCESS` or a negative error code (In particular, `RTC_ERR_NOT_AVAIL` is returned when there are no pending messages)

If `buffer` is `NULL`, the message is not copied and kept pending but the size is still written to `size`.

Track: By default, the track receives data as RTP packets.

#### rtcGetAvailableAmount

```
int rtcGetAvailableAmount(int id)
```

Retrieves the available amount, i.e. the total size of messages pending reception with `rtcReceiveMessage`. The function may only be called if `MessageCallback` is not set.

Arguments:

- `id`: the channel identifier

Return value: the available amount or a negative error code

### Data Channel

#### rtcCreateDataChannel

```
int rtcCreateDataChannel(int pc, const char *label)
int rtcCreateDataChannelEx(int pc, const char *label, const rtcDataChannelInit *init)

typedef struct {
	bool unordered;
	bool unreliable;
	unsigned int maxPacketLifeTime;
	unsigned int maxRetransmits;
} rtcReliability;

typedef struct {
	rtcReliability reliability;
	const char *protocol;
	bool negotiated;
	bool manualStream;
	uint16_t stream;
} rtcDataChannelInit;
```

Adds a Data Channel on a Peer Connection. The Peer Connection does not need to be connected, however, the Data Channel will be open only when the Peer Connection is connected.

Arguments:

- `pc`: identifier of the PeerConnection on which to add a Data Channel
- `label`: a user-defined UTF-8 string representing the Data Channel name
- `init`: a structure of initialization settings containing:
  - `reliability`: a structure of reliability settings containing:
    - `unordered`: if `true`, the Data Channel will not enforce message ordering, else it will be ordered
    - `unreliable`: if `true`, the Data Channel will not enforce strict reliability, else it will be reliable
    - `maxPacketLifeTime`: if unreliable, time window in milliseconds during which transmissions and retransmissions may occur
    - `maxRetransmits`: if unreliable and maxPacketLifeTime is 0, maximum number of attempted retransmissions (0 means no retransmission)
  - `protocol` (optional): a user-defined UTF-8 string representing the Data Channel protocol, empty if NULL
  - `negotiated`: if `true`, the Data Channel is assumed to be negotiated by the user and won't be negotiated by the WebRTC layer
  - `manualStream`: if `true`, the Data Channel will use `stream` as stream ID, else an available id is automatically selected
  - `stream`: if `manualStream` is `true`, the Data Channel will use it as stream ID, else it is ignored

`rtcDataChannel()` is equivalent to `rtcDataChannelEx()` with settings set to ordered, reliable, non-negotiated, with automatic stream ID selection (all flags set to `false`), and `protocol` set to an empty string.

Return value: the identifier of the new Data Channel or a negative error code.

The Data Channel must be deleted with `rtcDeleteDataChannel` (or `rtcDelete`).

If `disableAutoNegotiation` was not set in `rtcConfiguration`, the library will automatically initiate the negotiation by calling `rtcSetLocalDescription` internally. Otherwise, the user must call `rtcSetLocalDescription` to initiate the negotiation after creating the first Data Channel.

#### rtcDeleteDataChannel

```
int rtcDeleteDataChannel(int dc)
```

Deletes a Data Channel.

Arguments:

- `dc`: the Data Channel identifier

After this function has been called, `dc` must not be used in a function call anymore. This function will block until all scheduled callbacks of `dc` return (except the one this function might be called in) and no other callback will be called for `dc` after it returns.

#### rtcGetDataChannelStream

```
int rtcGetDataChannelStream(int dc)
```

Retrieves the stream ID of the Data Channel.

Arguments:

- `dc`: the Data Channel identifier

Return value: the stream ID or a negative error code

#### rtcGetDataChannelLabel

```
int rtcGetDataChannelLabel(int dc, char *buffer, int size)
```

Retrieves the label of a Data Channel.

Arguments:

- `dc`: the Data Channel identifier
- `buffer`: a user-supplied buffer to store the label
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the label is not copied but the size is still returned.

#### rtcGetDataChannelProtocol

```
int rtcGetDataChannelProtocol(int dc, char *buffer, int size)
```

Retrieves the protocol of a Data Channel.

Arguments:

- `dc`: the Data Channel identifier
- `buffer`: a user-supplied buffer to store the protocol
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the protocol is not copied but the size is still returned.

#### rtcGetDataChannelReliability

```
int rtcGetDataChannelReliability(int dc, rtcReliability *reliability)
```

Retrieves the reliability settings of a Data Channel. The function may be called irrelevant of how the Data Channel was created.

Arguments:

- `dc`: the Data Channel identifier
- `reliability` a user-supplied structure to fill

Return value: `RTC_ERR_SUCCESS` or a negative error code

### Track

#### rtcAddTrack

```
int rtcAddTrack(int pc, const char *mediaDescriptionSdp)
```

Adds a new Track on a Peer Connection. The Peer Connection does not need to be connected, however, the Track will be open only when the Peer Connection is connected.

Arguments:

- `pc`: the Peer Connection identifier
- `mediaDescriptionSdp`: a null-terminated string specifying the corresponding media SDP. It must start with a m-line and include a mid parameter.

Return value: the identifier of the new Track or a negative error code

The new track must be deleted with `rtcDeleteTrack` (or `rtcDelete`).

The user must call `rtcSetLocalDescription` to negotiate the track.

#### rtcDeleteTrack

```
int rtcDeleteTrack(int tr)
```

Deletes a Track.

Arguments:

- `tr`: the Track identifier

After this function has been called, `tr` must not be used in a function call anymore. This function will block until all scheduled callbacks of `tr` return (except the one this function might be called in) and no other callback will be called for `tr` after it returns.

#### rtcGetTrackDescription

```
int rtcGetTrackDescription(int tr, char *buffer, int size)
```

Retrieves the SDP media description of a Track.

Arguments:

- `tr`: the Track identifier
- `buffer`: a user-supplied buffer to store the description
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the description is not copied but the size is still returned.

#### rtcGetTrackMid

```
int rtcGetTrackMid(int tr, char *buffer, int size)
```

Retrieves the mid (media indentifier) of a Track.

Arguments:

- `tr`: the Track identifier
- `buffer`: a user-supplied buffer to store the mid
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the mid is not copied but the size is still returned.

#### rtcGetTrackDirection

```
int rtcGetTrackDirection(int tr, rtcDirection *direction)
```

Retrieves the direction of a Track.

Arguments:

- `tr`: the Track identifier
- `direction`: a pointer to a rtcDescription enum to store the result

On success, the value pointed by `direction` will be set to one of the following: `RTC_DIRECTION_SENDONLY`, `RTC_DIRECTION_RECVONLY`, `RTC_DIRECTION_SENDRECV`, `RTC_DIRECTION_INACTIVE`, or `RTC_DIRECTION_UNKNOWN`.

### Media handling

TODO

### WebSocket

#### rtcCreateWebSocket

```
int rtcCreateWebSocket(const char *url)
int rtcCreateWebSocketEx(const char *url, const rtcWsConfiguration *config)

typedef struct {
	bool disableTlsVerification;
	const char **protocols;
	int protocolsCount;
	int connectionTimeoutMs;
	int pingIntervalMs;
	int maxOutstandingPings;
} rtcWsConfiguration;
```

Creates a new client WebSocket.

Arguments:

- `url`: a null-terminated string representing the fully-qualified URL to open.
- `config`: a structure with the following parameters:
  - `disableTlsVerification`: if true, don't verify the TLS certificate, else try to verify it if possible
  - `protocols` (optional): an array of pointers on null-terminated protocol names (NULL if unused)
  - `protocolsCount` (optional): number of URLs in the array pointed by `protocols` (0 if unused)
  - `connectionTimeoutMs` (optional): connection timeout in milliseconds (0 if default, < 0 if disabled)
  - `pingIntervalMs` (optional): ping interval in milliseconds (0 if default, < 0 if disabled)
  - `maxOutstandingPings` (optional): number of unanswered pings before declaring failure (0 if default, < 0 if disabled)

Return value: the identifier of the new WebSocket or a negative error code

The new WebSocket must be deleted with `rtcDeleteWebSocket` (or `rtcDelete`). The scheme of the URL must be either `ws` or `wss`.

#### rtcDeleteWebSocket

```
int rtcDeleteWebSocket(int ws)
```

Arguments:

- `ws`: the identifier of the WebSocket to delete

After this function has been called, `ws` must not be used in a function call anymore. This function will block until all scheduled callbacks of `ws` return (except the one this function might be called in) and no other callback will be called for `ws` after it returns.

#### rtcGetWebSocketRemoteAddress

```
int rtcGetWebSocketRemoteAddress(int ws, char *buffer, int size)
```

Retrieves the remote address, i.e. the network address of the remote endpoint. The address will have the format `"HOST:PORT"`. The call may fail if the underlying TCP transport of the WebSocket is not connected. This function is useful for a client WebSocket received by a WebSocket Server.

Arguments:

- `ws`: the identifier of the WebSocket
- `buffer`: a user-supplied buffer to store the description
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the address is not copied but the size is still returned.

#### rtcGetWebSocketPath

```
int rtcGetWebSocketPath(int ws, char *buffer, int size)
```

Retrieves the path of the WebSocket, i.e. the HTTP requested path. This function is useful for a client WebSocket received by a WebSocket Server. Warning: The WebSocket must be open for the call to succeed.

Arguments:

- `ws`: the identifier of the WebSocket
- `buffer`: a user-supplied buffer to store the description
- `size`: the size of `buffer`

Return value: the length of the string copied in buffer (including the terminating null character) or a negative error code

If `buffer` is `NULL`, the path is not copied but the size is still returned.

### WebSocket Server

#### rtcCreateWebSocketServer

```
int rtcCreateWebSocketServer(const rtcWsServerConfiguration *config, rtcWebSocketClientCallbackFunc cb);

typedef struct {
	uint16_t port;
	bool enableTls;
	const char *certificatePemFile;
	const char *keyPemFile;
	const char *keyPemPass;
	int connectionTimeoutMs;
} rtcWsServerConfiguration;
```

Creates a new WebSocket server.

Arguments:

- `config`: a structure with the following parameters:
  - `port`: the port to listen on (if 0, automatically select an available port)
  - `enableTls`: if true, enable the TLS layer (WSS)
  - `certificatePemFile` (optional): PEM certificate or path of the file containing the PEM certificate (`NULL` for an autogenerated certificate)
  - `keyPemFile` (optional): PEM key or path of the file containing the PEM key (`NULL` for an autogenerated certificate)
  - `keyPemPass` (optional): PEM key file passphrase (NULL if no passphrase)
  - `connectionTimeoutMs` (optional): connection timeout in milliseconds (0 if default, < 0 if disabled)
- `cb`: the callback for incoming client WebSocket connections (must not be `NULL`)

`cb` must have the following signature: `void rtcWebSocketClientCallbackFunc(int wsserver, int ws, void *user_ptr)`

Return value: the identifier of the new WebSocket Server or a negative error code

The new WebSocket Server must be deleted with `rtcDeleteWebSocketServer`.

#### rtcDeleteWebSocketServer

```
int rtcDeleteWebSocketServer(int wsserver)
```

Arguments:

- `wsserver`: the identifier of the WebSocket Server to delete

After this function has been called, `wsserver` must not be used in a function call anymore. This function will block until all scheduled callbacks of `wsserver` return (except the one this function might be called in) and no other callback will be called for `wsserver` after it returns.

#### rtcGetWebSocketServerPort
```
int rtcGetWebSocketServerPort(int wsserver);
```

Retrieves the port which the WebSocket Server is listening on.

Arguments:

- `wsserver`: the identifier of the WebSocket Server

Return value: The port of the WebSocket Server or a negative error code


