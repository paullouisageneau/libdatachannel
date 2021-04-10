# libdatachannel - client-benchmark

This directory contains a native client to open Data Channels with WebSocket signaling using libdatachannel and benchmark functionalities. It offers three functionalities;
- Benchmark: Bi-directional data transfer benchmark (Also supports One-Way testing)
- Constant Throughput Set: Send desired amount of data per second
- Multiple Data Channel: Create desried count of data channel 

## Start Signaling Server
- Start one of the signaling server from the examples folder. For example start  `signaling-server-nodejs` like;
  - `cd examples/signaling-server-nodejs/`
  - `npm i`
  - `npm run start `

## Start `client-benchmark` Applications

Start 2 applications by using example calls below. Then copy one of the client's ID and paste to the other peer's screen to start offering process.

## Usage Examples

### Benchmark for 300 seconds

> `./client-benchmark -d 300` 

Example Output (Offering Peer's Output);
```bash
Stun server is stun:stun.l.google.com:19302
The local ID is: H1E3
Url is ws://localhost:8000/H1E3
Waiting for signaling to be connected...
2021-04-10 19:51:31.319 INFO  [16449] [rtc::impl::TcpTransport::connect@163] Connected to localhost:8000
2021-04-10 19:51:31.319 INFO  [16449] [rtc::impl::TcpTransport::runLoop@331] TCP connected
2021-04-10 19:51:31.321 INFO  [16449] [rtc::impl::WsTransport::incoming@118] WebSocket open
WebSocket connected, signaling ready
Enter a remote ID to send an offer:
n790
Offering to n790
Creating DataChannel with label "DC-1"
2021-04-10 19:51:32.464 INFO  [16442] [rtc::impl::IceTransport::IceTransport@106] Using STUN server "stun.l.google.com:19302"
2021-04-10 19:51:32.465 INFO  [16442] [rtc::impl::PeerConnection::changeSignalingState@1044] Changed signaling state to new
2021-04-10 19:51:32.465 INFO  [16442] [rtc::impl::PeerConnection::changeGatheringState@1033] Changed gathering state to in-progress
Gathering State: in-progress
2021-04-10 19:51:32.465 INFO  [16442] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to gathering
Benchmark will run for 300 seconds
2021-04-10 19:51:32.466 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connecting
2021-04-10 19:51:32.466 INFO  [16450] [rtc::impl::PeerConnection::changeState@1016] Changed state to connecting
State: connecting
2021-04-10 19:51:32.489 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:610: Using STUN server stun.l.google.com:19302
2021-04-10 19:51:32.489 INFO  [16449] [rtc::impl::PeerConnection::changeSignalingState@1044] Changed signaling state to connecting
2021-04-10 19:51:32.490 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connected
2021-04-10 19:51:32.491 INFO  [16453] [rtc::impl::DtlsTransport::runRecvLoop@503] DTLS handshake finished
2021-04-10 19:51:32.497 INFO  [16443] [rtc::impl::SctpTransport::processNotification@713] SCTP connected
2021-04-10 19:51:32.497 INFO  [16443] [rtc::impl::PeerConnection::changeState@1016] Changed state to connected
State: connected
DataChannel from n790 open
2021-04-10 19:51:32.542 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1328: STUN server binding successful
2021-04-10 19:51:32.589 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to completed
#1
      DC-1 Received: 40789 KB/s   Sent: 41180 KB/s   BufferSize: 65535
      TOTL Received: 40789 KB/s   Sent: 41180 KB/s
2021-04-10 19:51:34.039 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:843: STUN server binding failed (timeout)
2021-04-10 19:51:34.039 INFO  [16450] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:2206: Candidate gathering done
2021-04-10 19:51:34.039 INFO  [16450] [rtc::impl::PeerConnection::changeGatheringState@1033] Changed gathering state to complete
Gathering State: complete
#2
      DC-1 Received: 41709 KB/s   Sent: 41774 KB/s   BufferSize: 65535
      TOTL Received: 41709 KB/s   Sent: 41774 KB/s
#3
      DC-1 Received: 42165 KB/s   Sent: 42360 KB/s   BufferSize: 65535
      TOTL Received: 42165 KB/s   Sent: 42360 KB/s
#4
      DC-1 Received: 42880 KB/s   Sent: 42750 KB/s   BufferSize: 65535
      TOTL Received: 42880 KB/s   Sent: 42750 KB/s
#5
      DC-1 Received: 41771 KB/s   Sent: 42097 KB/s   BufferSize: 65535
      TOTL Received: 41771 KB/s   Sent: 42097 KB/s
Stats# Received Total: 210 MB   Sent Total: 211 MB   RTT: 20 ms

#6
      DC-1 Received: 46235 KB/s   Sent: 30433 KB/s   BufferSize: 65535
      TOTL Received: 46235 KB/s   Sent: 30433 KB/s
#7
      DC-1 Received: 47116 KB/s   Sent: 28413 KB/s   BufferSize: 65535
      TOTL Received: 47116 KB/s   Sent: 28413 KB/s
#8
      DC-1 Received: 46923 KB/s   Sent: 32520 KB/s   BufferSize: 65535
      TOTL Received: 46923 KB/s   Sent: 32520 KB/s
#9
      DC-1 Received: 44513 KB/s   Sent: 34020 KB/s   BufferSize: 65535
      TOTL Received: 44513 KB/s   Sent: 34020 KB/s
#10
      DC-1 Received: 41966 KB/s   Sent: 36166 KB/s   BufferSize: 65535
      TOTL Received: 41966 KB/s   Sent: 36166 KB/s
Stats# Received Total: 438 MB   Sent Total: 373 MB   RTT: 19 ms

#11
      DC-1 Received: 42617 KB/s   Sent: 39619 KB/s   BufferSize: 65535
      TOTL Received: 42617 KB/s   Sent: 39619 KB/s
#12
      DC-1 Received: 43792 KB/s   Sent: 43338 KB/s   BufferSize: 65535
      TOTL Received: 43792 KB/s   Sent: 43338 KB/s
#13
      DC-1 Received: 41715 KB/s   Sent: 41585 KB/s   BufferSize: 65535
      TOTL Received: 41715 KB/s   Sent: 41585 KB/s
#14
      DC-1 Received: 39860 KB/s   Sent: 33822 KB/s   BufferSize: 65535
      TOTL Received: 39860 KB/s   Sent: 33822 KB/s
#15
      DC-1 Received: 47576 KB/s   Sent: 25352 KB/s   BufferSize: 65535
      TOTL Received: 47576 KB/s   Sent: 25352 KB/s
Stats# Received Total: 655 MB   Sent Total: 558 MB   RTT: 13 ms
```

### Benchmark for 300 seconds (Only Send, One Way)

Start first peer as;
> `./client-benchmark -d 300 -o` 

Start second peer as;
> `./client-benchmark -d 300` 

Example Output (Offering Peer's Output);
```bash
Not Sending data. (One way benchmark).
Stun server is stun:stun.l.google.com:19302
The local ID is: 7EaP
Url is ws://localhost:8000/7EaP
Waiting for signaling to be connected...
2021-04-10 19:54:36.857 INFO  [16632] [rtc::impl::TcpTransport::connect@163] Connected to localhost:8000
2021-04-10 19:54:36.857 INFO  [16632] [rtc::impl::TcpTransport::runLoop@331] TCP connected
2021-04-10 19:54:36.858 INFO  [16632] [rtc::impl::WsTransport::incoming@118] WebSocket open
WebSocket connected, signaling ready
Enter a remote ID to send an offer:
UDL4
Offering to UDL4
Creating DataChannel with label "DC-1"
2021-04-10 19:54:53.381 INFO  [16625] [rtc::impl::IceTransport::IceTransport@106] Using STUN server "stun.l.google.com:19302"
2021-04-10 19:54:53.382 INFO  [16625] [rtc::impl::PeerConnection::changeSignalingState@1044] Changed signaling state to new
2021-04-10 19:54:53.382 INFO  [16625] [rtc::impl::PeerConnection::changeGatheringState@1033] Changed gathering state to in-progress
Gathering State: in-progress
2021-04-10 19:54:53.383 INFO  [16625] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to gathering
Benchmark will run for 300 seconds
2021-04-10 19:54:53.384 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connecting
2021-04-10 19:54:53.384 INFO  [16646] [rtc::impl::PeerConnection::changeState@1016] Changed state to connecting
State: connecting
2021-04-10 19:54:53.475 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:610: Using STUN server stun.l.google.com:19302
2021-04-10 19:54:53.475 INFO  [16632] [rtc::impl::PeerConnection::changeSignalingState@1044] Changed signaling state to connecting
2021-04-10 19:54:53.527 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1328: STUN server binding successful
2021-04-10 19:54:53.575 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connected
2021-04-10 19:54:53.625 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to completed
#1
      DC-1 Received: 0 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 0 KB/s   Sent: 0 KB/s
2021-04-10 19:54:54.481 INFO  [16653] [rtc::impl::DtlsTransport::runRecvLoop@503] DTLS handshake finished
2021-04-10 19:54:54.491 INFO  [16627] [rtc::impl::SctpTransport::processNotification@713] SCTP connected
2021-04-10 19:54:54.491 INFO  [16627] [rtc::impl::PeerConnection::changeState@1016] Changed state to connected
State: connected
DataChannel from UDL4 open
#2
      DC-1 Received: 84326 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 84326 KB/s   Sent: 0 KB/s
#3
      DC-1 Received: 99387 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 99387 KB/s   Sent: 0 KB/s
2021-04-10 19:54:57.025 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:843: STUN server binding failed (timeout)
2021-04-10 19:54:57.025 INFO  [16646] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:2206: Candidate gathering done
2021-04-10 19:54:57.025 INFO  [16646] [rtc::impl::PeerConnection::changeGatheringState@1033] Changed gathering state to complete
Gathering State: complete
#4
      DC-1 Received: 94871 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 94871 KB/s   Sent: 0 KB/s
#5
      DC-1 Received: 96259 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 96259 KB/s   Sent: 0 KB/s
Stats# Received Total: 377 MB   Sent Total: 0 MB   RTT: 2 ms

#6
      DC-1 Received: 92873 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 92873 KB/s   Sent: 0 KB/s
#7
      DC-1 Received: 87724 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 87724 KB/s   Sent: 0 KB/s
#8
      DC-1 Received: 95123 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 95123 KB/s   Sent: 0 KB/s
#9
      DC-1 Received: 100022 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 100022 KB/s   Sent: 0 KB/s
#10
      DC-1 Received: 98124 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 98124 KB/s   Sent: 0 KB/s
Stats# Received Total: 853 MB   Sent Total: 0 MB   RTT: 2 ms

#11
      DC-1 Received: 103628 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 103628 KB/s   Sent: 0 KB/s
#12
      DC-1 Received: 106166 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 106166 KB/s   Sent: 0 KB/s
#13
      DC-1 Received: 98410 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 98410 KB/s   Sent: 0 KB/s
#14
      DC-1 Received: 99854 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 99854 KB/s   Sent: 0 KB/s
#15
      DC-1 Received: 98487 KB/s   Sent: 0 KB/s   BufferSize: 0
      TOTL Received: 98487 KB/s   Sent: 0 KB/s
Stats# Received Total: 1362 MB   Sent Total: 0 MB   RTT: 2 ms
```

### Constant Throughput Set 8000 byte, for 300 seconds, send buffer 10000 byte

> `./client-benchmark -p -d 300 -r 8000 -b 10000` 

Example Output (Offering Peer's Output);
```bash
Stun server is stun:stun.l.google.com:19302
The local ID is: 5zkC
Url is ws://localhost:8000/5zkC
Waiting for signaling to be connected...
2021-04-10 19:52:49.788 INFO  [16530] [rtc::impl::TcpTransport::connect@163] Connected to localhost:8000
2021-04-10 19:52:49.788 INFO  [16530] [rtc::impl::TcpTransport::runLoop@331] TCP connected
2021-04-10 19:52:49.789 INFO  [16530] [rtc::impl::WsTransport::incoming@118] WebSocket open
WebSocket connected, signaling ready
Enter a remote ID to send an offer:
WawD
Offering to WawD
Creating DataChannel with label "DC-1"
2021-04-10 19:52:57.720 INFO  [16523] [rtc::impl::IceTransport::IceTransport@106] Using STUN server "stun.l.google.com:19302"
2021-04-10 19:52:57.721 INFO  [16523] [rtc::impl::PeerConnection::changeSignalingState@1044] Changed signaling state to new
2021-04-10 19:52:57.721 INFO  [16523] [rtc::impl::PeerConnection::changeGatheringState@1033] Changed gathering state to in-progress
Gathering State: in-progress
2021-04-10 19:52:57.722 INFO  [16523] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to gathering
Benchmark will run for 300 seconds
2021-04-10 19:52:57.722 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connecting
2021-04-10 19:52:57.722 INFO  [16533] [rtc::impl::PeerConnection::changeState@1016] Changed state to connecting
State: connecting
2021-04-10 19:52:57.725 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:610: Using STUN server stun.l.google.com:19302
2021-04-10 19:52:57.727 INFO  [16530] [rtc::impl::PeerConnection::changeSignalingState@1044] Changed signaling state to connecting
2021-04-10 19:52:57.826 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connected
2021-04-10 19:52:57.828 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to completed
2021-04-10 19:52:57.829 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1328: STUN server binding successful
2021-04-10 19:52:57.884 INFO  [16535] [rtc::impl::DtlsTransport::runRecvLoop@503] DTLS handshake finished
2021-04-10 19:52:57.907 INFO  [16526] [rtc::impl::SctpTransport::processNotification@713] SCTP connected
2021-04-10 19:52:57.907 INFO  [16526] [rtc::impl::PeerConnection::changeState@1016] Changed state to connected
State: connected
DataChannel from WawD open
#1
      DC-1 Received: 6515 KB/s   Sent: 6577 KB/s   BufferSize: 0
      TOTL Received: 6515 KB/s   Sent: 6577 KB/s
#2
      DC-1 Received: 7998 KB/s   Sent: 7999 KB/s   BufferSize: 0
      TOTL Received: 7998 KB/s   Sent: 7999 KB/s
#3
      DC-1 Received: 7933 KB/s   Sent: 7999 KB/s   BufferSize: 0
      TOTL Received: 7933 KB/s   Sent: 7999 KB/s
2021-04-10 19:53:01.275 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:843: STUN server binding failed (timeout)
2021-04-10 19:53:01.275 INFO  [16533] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:2206: Candidate gathering done
2021-04-10 19:53:01.275 INFO  [16533] [rtc::impl::PeerConnection::changeGatheringState@1033] Changed gathering state to complete
Gathering State: complete
#4
      DC-1 Received: 8070 KB/s   Sent: 8000 KB/s   BufferSize: 0
      TOTL Received: 8070 KB/s   Sent: 8000 KB/s
#5
      DC-1 Received: 7984 KB/s   Sent: 8000 KB/s   BufferSize: 0
      TOTL Received: 7984 KB/s   Sent: 8000 KB/s
Stats# Received Total: 39 MB   Sent Total: 39 MB   RTT: 0 ms

#6
      DC-1 Received: 8004 KB/s   Sent: 7998 KB/s   BufferSize: 0
      TOTL Received: 8004 KB/s   Sent: 7998 KB/s
#7
      DC-1 Received: 7997 KB/s   Sent: 8000 KB/s   BufferSize: 0
      TOTL Received: 7997 KB/s   Sent: 8000 KB/s
#8
      DC-1 Received: 8008 KB/s   Sent: 8000 KB/s   BufferSize: 0
      TOTL Received: 8008 KB/s   Sent: 8000 KB/s
#9
      DC-1 Received: 8007 KB/s   Sent: 8000 KB/s   BufferSize: 0
      TOTL Received: 8007 KB/s   Sent: 8000 KB/s
#10
      DC-1 Received: 7999 KB/s   Sent: 7999 KB/s   BufferSize: 0
      TOTL Received: 7999 KB/s   Sent: 7999 KB/s
Stats# Received Total: 81 MB   Sent Total: 81 MB   RTT: 0 ms

#11
      DC-1 Received: 7997 KB/s   Sent: 8001 KB/s   BufferSize: 0
      TOTL Received: 7997 KB/s   Sent: 8001 KB/s
#12
      DC-1 Received: 7981 KB/s   Sent: 7997 KB/s   BufferSize: 0
      TOTL Received: 7981 KB/s   Sent: 7997 KB/s
#13
      DC-1 Received: 8024 KB/s   Sent: 8000 KB/s   BufferSize: 0
      TOTL Received: 8024 KB/s   Sent: 8000 KB/s
#14
      DC-1 Received: 7990 KB/s   Sent: 7999 KB/s   BufferSize: 0
      TOTL Received: 7990 KB/s   Sent: 7999 KB/s
#15
      DC-1 Received: 8001 KB/s   Sent: 8002 KB/s   BufferSize: 0
      TOTL Received: 8001 KB/s   Sent: 8002 KB/s
Stats# Received Total: 122 MB   Sent Total: 122 MB   RTT: 0 ms
```