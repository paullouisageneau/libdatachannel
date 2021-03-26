# libdatachannel - client-benchmark

This directory contains a native client to open Data Channels with WebSocket signaling using libdatachannel and benchmark functionalities. It offers two functionalities;
- Benchmark: Bi-directional data transfer benchmark (Also supports One-Way testing)
- Constant Throughput Set: Send desired amount of data per second

## Start Signaling Server
- Start one of the signaling server from the examples folder. For example start  `signaling-server-nodejs` like;
  - `cd examples/signaling-server-nodejs/`
  - `npm i`
  - `npm run start `

## Start `client-benchmark` Applications

Start 2 applications by using example calls below. Than copy one of the client's ID and paste to the other peer's screen to start offering process.

## Usage Examples

### Benchmark for 300 seconds

> `./client-benchmark -d 300` 

Example Output (Offering Peer's Output);
```bash
Stun server is stun:stun.l.google.com:19302
The local ID is: EQmF
Url is ws://localhost:8000/EQmF
Waiting for signaling to be connected...
2021-03-25 14:21:58.045 INFO  [21386] [rtc::impl::TcpTransport::connect@159] Connected to localhost:8000
2021-03-25 14:21:58.045 INFO  [21386] [rtc::impl::TcpTransport::runLoop@327] TCP connected
2021-03-25 14:21:58.046 INFO  [21386] [rtc::impl::WsTransport::incoming@118] WebSocket open
WebSocket connected, signaling ready
Enter a remote ID to send an offer:
0tDf
Offering to 0tDf
Creating DataChannel with label "benchmark"
2021-03-25 14:22:07.972 INFO  [21379] [rtc::impl::IceTransport::IceTransport@106] Using STUN server "stun.l.google.com:19302"
2021-03-25 14:22:07.973 INFO  [21379] [rtc::impl::PeerConnection::changeSignalingState@992] Changed signaling state to new
2021-03-25 14:22:07.973 INFO  [21379] [rtc::impl::PeerConnection::changeGatheringState@981] Changed gathering state to in-progress
Gathering State: in-progress
2021-03-25 14:22:07.974 INFO  [21379] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to gathering
2021-03-25 14:22:07.974 WARN  [21379] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:239: Local description already has the maximum number of host candidates
Benchmark will run for 300 seconds
2021-03-25 14:22:07.976 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connecting
2021-03-25 14:22:07.976 INFO  [21396] [rtc::impl::PeerConnection::changeState@964] Changed state to connecting
State: connecting
2021-03-25 14:22:08.055 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:610: Using STUN server stun.l.google.com:19302
2021-03-25 14:22:08.055 INFO  [21386] [rtc::impl::PeerConnection::changeSignalingState@992] Changed signaling state to connecting
2021-03-25 14:22:08.105 WARN  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:426: Send failed, errno=101
2021-03-25 14:22:08.105 WARN  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1483: STUN message send failed, errno=101
2021-03-25 14:22:08.105 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:843: STUN binding failed
2021-03-25 14:22:08.107 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1302: STUN server binding successful
2021-03-25 14:22:08.107 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:2148: Candidate gathering done
2021-03-25 14:22:08.107 INFO  [21396] [rtc::impl::PeerConnection::changeGatheringState@981] Changed gathering state to complete
Gathering State: complete
2021-03-25 14:22:08.155 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connected
2021-03-25 14:22:08.206 INFO  [21396] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to completed
#1 Received: 0 KB/s   Sent: 0 KB/s   BufferSize: 0
2021-03-25 14:22:09.059 INFO  [21399] [rtc::impl::DtlsTransport::runRecvLoop@503] DTLS handshake finished
2021-03-25 14:22:09.069 INFO  [21382] [rtc::impl::SctpTransport::processNotification@708] SCTP connected
2021-03-25 14:22:09.069 INFO  [21382] [rtc::impl::PeerConnection::changeState@964] Changed state to connected
State: connected
DataChannel from 0tDf open
#2 Received: 41488 KB/s   Sent: 42465 KB/s   BufferSize: 65535
#3 Received: 43925 KB/s   Sent: 43729 KB/s   BufferSize: 65535
#4 Received: 42491 KB/s   Sent: 42361 KB/s   BufferSize: 65535
#5 Received: 45878 KB/s   Sent: 45682 KB/s   BufferSize: 65535
Stats# Received Total: 174 MB   Sent Total: 175 MB   RTT: 17 ms

#6 Received: 43665 KB/s   Sent: 43599 KB/s   BufferSize: 65535
#7 Received: 45749 KB/s   Sent: 45488 KB/s   BufferSize: 65535
#8 Received: 44055 KB/s   Sent: 44055 KB/s   BufferSize: 65535
#9 Received: 21572 KB/s   Sent: 58199 KB/s   BufferSize: 65535
#10 Received: 22940 KB/s   Sent: 55005 KB/s   BufferSize: 65535
Stats# Received Total: 353 MB   Sent Total: 422 MB   RTT: 15 ms

#11 Received: 27501 KB/s   Sent: 53112 KB/s   BufferSize: 65535
#12 Received: 29914 KB/s   Sent: 48162 KB/s   BufferSize: 65535
#13 Received: 31869 KB/s   Sent: 45946 KB/s   BufferSize: 65535
#14 Received: 22484 KB/s   Sent: 53636 KB/s   BufferSize: 65535
#15 Received: 16294 KB/s   Sent: 56833 KB/s   BufferSize: 65535
Stats# Received Total: 482 MB   Sent Total: 682 MB   RTT: 13 ms
```

### Benchmark for 300 seconds (Only Send, One Way)

Start first peer as;
> `./client-benchmark -d 300 -o` 

Start second peer as;
> `./client-benchmark -d 300` 

Example Output (Offering Peer's Output);
```bash
Stun server is stun:stun.l.google.com:19302
The local ID is: XLLn
Url is ws://localhost:8000/XLLn
Waiting for signaling to be connected...
2021-03-25 14:34:24.479 INFO  [22332] [rtc::impl::TcpTransport::connect@159] Connected to localhost:8000
2021-03-25 14:34:24.479 INFO  [22332] [rtc::impl::TcpTransport::runLoop@327] TCP connected
2021-03-25 14:34:24.479 INFO  [22332] [rtc::impl::WsTransport::incoming@118] WebSocket open
WebSocket connected, signaling ready
Enter a remote ID to send an offer:
Okt4
Offering to Okt4
Creating DataChannel with label "benchmark"
2021-03-25 14:34:37.948 INFO  [22325] [rtc::impl::IceTransport::IceTransport@106] Using STUN server "stun.l.google.com:19302"
2021-03-25 14:34:37.949 INFO  [22325] [rtc::impl::PeerConnection::changeSignalingState@992] Changed signaling state to new
2021-03-25 14:34:37.949 INFO  [22325] [rtc::impl::PeerConnection::changeGatheringState@981] Changed gathering state to in-progress
Gathering State: in-progress
2021-03-25 14:34:37.950 INFO  [22325] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to gathering
2021-03-25 14:34:37.951 WARN  [22325] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:239: Local description already has the maximum number of host candidates
Benchmark will run for 300 seconds
2021-03-25 14:34:37.952 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connecting
2021-03-25 14:34:37.952 INFO  [22334] [rtc::impl::PeerConnection::changeState@964] Changed state to connecting
State: connecting
2021-03-25 14:34:37.969 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:610: Using STUN server stun.l.google.com:19302
2021-03-25 14:34:37.969 INFO  [22332] [rtc::impl::PeerConnection::changeSignalingState@992] Changed signaling state to connecting
2021-03-25 14:34:37.970 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connected
2021-03-25 14:34:37.971 INFO  [22337] [rtc::impl::DtlsTransport::runRecvLoop@503] DTLS handshake finished
2021-03-25 14:34:37.977 INFO  [22327] [rtc::impl::SctpTransport::processNotification@708] SCTP connected
2021-03-25 14:34:37.977 INFO  [22327] [rtc::impl::PeerConnection::changeState@964] Changed state to connected
State: connected
DataChannel from Okt4 open
2021-03-25 14:34:38.019 WARN  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:426: Send failed, errno=101
2021-03-25 14:34:38.019 WARN  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1483: STUN message send failed, errno=101
2021-03-25 14:34:38.019 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:843: STUN binding failed
2021-03-25 14:34:38.022 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1302: STUN server binding successful
2021-03-25 14:34:38.022 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:2148: Candidate gathering done
2021-03-25 14:34:38.022 INFO  [22334] [rtc::impl::PeerConnection::changeGatheringState@981] Changed gathering state to complete
Gathering State: complete
2021-03-25 14:34:38.069 INFO  [22334] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to completed
#1 Received: 0 KB/s   Sent: 92223 KB/s   BufferSize: 65535
#2 Received: 0 KB/s   Sent: 89291 KB/s   BufferSize: 65535
#3 Received: 0 KB/s   Sent: 95087 KB/s   BufferSize: 65535
#4 Received: 0 KB/s   Sent: 101050 KB/s   BufferSize: 65535
#5 Received: 0 KB/s   Sent: 99778 KB/s   BufferSize: 0
Stats# Received Total: 0 MB   Sent Total: 480 MB   RTT: 8 ms

#6 Received: 0 KB/s   Sent: 100366 KB/s   BufferSize: 65535
#7 Received: 0 KB/s   Sent: 101201 KB/s   BufferSize: 65535
#8 Received: 0 KB/s   Sent: 100892 KB/s   BufferSize: 65535
#9 Received: 0 KB/s   Sent: 101288 KB/s   BufferSize: 65535
#10 Received: 0 KB/s   Sent: 95676 KB/s   BufferSize: 65535
Stats# Received Total: 0 MB   Sent Total: 982 MB   RTT: 8 ms

#11 Received: 0 KB/s   Sent: 96254 KB/s   BufferSize: 65535
#12 Received: 0 KB/s   Sent: 105473 KB/s   BufferSize: 65535
#13 Received: 0 KB/s   Sent: 95549 KB/s   BufferSize: 65535
#14 Received: 0 KB/s   Sent: 100366 KB/s   BufferSize: 65535
#15 Received: 0 KB/s   Sent: 101336 KB/s   BufferSize: 65535
Stats# Received Total: 0 MB   Sent Total: 1484 MB   RTT: 8 ms
```

### Constant Throughput Set 8000 byte, for 300 seconds, send buffer 10000 byte

> `./client-benchmark -p -d 300 -r 8000 -b 10000` 

Example Output (Offering Peer's Output);
```bash
Stun server is stun:stun.l.google.com:19302
The local ID is: 1w9O
Url is ws://localhost:8000/1w9O
Waiting for signaling to be connected...
2021-03-25 14:29:38.697 INFO  [21844] [rtc::impl::TcpTransport::connect@159] Connected to localhost:8000
2021-03-25 14:29:38.697 INFO  [21844] [rtc::impl::TcpTransport::runLoop@327] TCP connected
2021-03-25 14:29:38.698 INFO  [21844] [rtc::impl::WsTransport::incoming@118] WebSocket open
WebSocket connected, signaling ready
Enter a remote ID to send an offer:
zi4B
Offering to zi4B
Creating DataChannel with label "benchmark"
2021-03-25 14:29:48.729 INFO  [21837] [rtc::impl::IceTransport::IceTransport@106] Using STUN server "stun.l.google.com:19302"
2021-03-25 14:29:48.729 INFO  [21837] [rtc::impl::PeerConnection::changeSignalingState@992] Changed signaling state to new
2021-03-25 14:29:48.729 INFO  [21837] [rtc::impl::PeerConnection::changeGatheringState@981] Changed gathering state to in-progress
Gathering State: in-progress
2021-03-25 14:29:48.729 INFO  [21837] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to gathering
2021-03-25 14:29:48.730 WARN  [21837] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:239: Local description already has the maximum number of host candidates
Benchmark will run for 300 seconds
2021-03-25 14:29:48.730 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connecting
2021-03-25 14:29:48.731 INFO  [21866] [rtc::impl::PeerConnection::changeState@964] Changed state to connecting
State: connecting
2021-03-25 14:29:48.732 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:610: Using STUN server stun.l.google.com:19302
2021-03-25 14:29:48.732 INFO  [21844] [rtc::impl::PeerConnection::changeSignalingState@992] Changed signaling state to connecting
2021-03-25 14:29:48.782 WARN  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:426: Send failed, errno=101
2021-03-25 14:29:48.782 WARN  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1483: STUN message send failed, errno=101
2021-03-25 14:29:48.782 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:843: STUN binding failed
2021-03-25 14:29:48.787 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:1302: STUN server binding successful
2021-03-25 14:29:48.787 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:2148: Candidate gathering done
2021-03-25 14:29:48.787 INFO  [21866] [rtc::impl::PeerConnection::changeGatheringState@981] Changed gathering state to complete
Gathering State: complete
2021-03-25 14:29:48.832 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to connected
2021-03-25 14:29:48.882 INFO  [21866] [rtc::impl::IceTransport::LogCallback@339] juice: agent.c:787: Changing state to completed
2021-03-25 14:29:49.735 INFO  [21869] [rtc::impl::DtlsTransport::runRecvLoop@503] DTLS handshake finished
2021-03-25 14:29:49.742 INFO  [21841] [rtc::impl::SctpTransport::processNotification@708] SCTP connected
2021-03-25 14:29:49.742 INFO  [21841] [rtc::impl::PeerConnection::changeState@964] Changed state to connected
State: connected
DataChannel from zi4B open
#1 Received: 0 KB/s   Sent: 78 KB/s   BufferSize: 0
#2 Received: 8002 KB/s   Sent: 7999 KB/s   BufferSize: 0
#3 Received: 8002 KB/s   Sent: 7998 KB/s   BufferSize: 0
#4 Received: 7995 KB/s   Sent: 8000 KB/s   BufferSize: 0
#5 Received: 8000 KB/s   Sent: 8001 KB/s   BufferSize: 0
Stats# Received Total: 33 MB   Sent Total: 33 MB   RTT: 0 ms

#6 Received: 8001 KB/s   Sent: 7999 KB/s   BufferSize: 0
#7 Received: 7997 KB/s   Sent: 7998 KB/s   BufferSize: 0
#8 Received: 8001 KB/s   Sent: 7999 KB/s   BufferSize: 0
#9 Received: 7998 KB/s   Sent: 8001 KB/s   BufferSize: 0
#10 Received: 8003 KB/s   Sent: 7998 KB/s   BufferSize: 0
Stats# Received Total: 74 MB   Sent Total: 74 MB   RTT: 0 ms

#11 Received: 7990 KB/s   Sent: 7998 KB/s   BufferSize: 0
#12 Received: 7999 KB/s   Sent: 8000 KB/s   BufferSize: 0
#13 Received: 8001 KB/s   Sent: 8002 KB/s   BufferSize: 0
#14 Received: 7998 KB/s   Sent: 7999 KB/s   BufferSize: 0
#15 Received: 8000 KB/s   Sent: 7998 KB/s   BufferSize: 0
Stats# Received Total: 115 MB   Sent Total: 115 MB   RTT: 0 ms
```