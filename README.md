# libdatachannel - C/C++ WebRTC Data Channels

libdatachannel is a standalone implementation of WebRTC Data Channels and WebSockets in C++17 with C bindings for POSIX platforms (including Linux and Apple macOS) and Microsoft Windows. It enables direct connectivity between native applications and web browsers without the pain of importing the entire WebRTC stack. Its API is modelled as a simplified version of the JavaScript WebRTC and WebSocket API, in order to ease the design of cross-environment applications.

It can be compiled with multiple backends:
- The security layer can be provided through [GnuTLS](https://www.gnutls.org/) or [OpenSSL](https://www.openssl.org/).
- The connectivity for WebRTC can be provided through my ad-hoc ICE library [libjuice](https://github.com/paullouisageneau/libjuice) as submodule or through [libnice](https://github.com/libnice/libnice).

This projet is originally inspired by [librtcdcpp](https://github.com/chadnickbok/librtcdcpp), however it is a complete rewrite from scratch, because the messy architecture of librtcdcpp made solving its implementation issues difficult.

Licensed under LGPLv2, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE).

## Compatibility

The library aims at implementing the following communication protocols:

### WebRTC Data Channel

Protocol stack:
- SCTP-based Data Channels ([draft-ietf-rtcweb-data-channel-13](https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13))
- DTLS/UDP ([RFC7350](https://tools.ietf.org/html/rfc7350) and [RFC8261](https://tools.ietf.org/html/rfc8261))
- ICE ([RFC8445](https://tools.ietf.org/html/rfc8445)) with STUN [RFC5389](https://tools.ietf.org/html/rfc5389)

Features:
- IPv6 support
- Trickle ICE [draft-ietf-ice-trickle-21](https://tools.ietf.org/html/draft-ietf-ice-trickle-21)
- Multicast DNS candidates resolution ([draft-ietf-rtcweb-mdns-ice-candidates-03](https://tools.ietf.org/html/draft-ietf-rtcweb-mdns-ice-candidates-03)) provided the operating system has mDNS support
- TURN relaying [RFC5766](https://tools.ietf.org/html/rfc5766) with [libnice](https://github.com/libnice/libnice) as ICE backend.

It has been tested to be compatible with Firefox and Chromium.

### WebSocket (optional)

Protocol stack:
- WebSocket protocol ([RFC6455](https://tools.ietf.org/html/rfc6455)), client-side only
- HTTP over TLS ([RFC2818](https://tools.ietf.org/html/rfc2818))

Features:
- IPv6 support
- Keepalive with ping/pong

WebSocket is the protocol of choice for WebRTC signaling.

## Dependencies

- GnuTLS: https://www.gnutls.org/ or OpenSSL: https://www.openssl.org/

Optional:
- libnice: https://nice.freedesktop.org/ (substituable with libjuice)

Submodules:
- usrsctp: https://github.com/sctplab/usrsctp
- libjuice: https://github.com/paullouisageneau/libjuice

## Building
### Building with CMake (preferred)

```bash
$ git submodule update --init --recursive
$ mkdir build
$ cd build
$ cmake -DUSE_JUICE=1 -DUSE_GNUTLS=1 ..
$ make
```

### Building directly with Make

```bash
$ git submodule update --init --recursive
$ make USE_JUICE=1 USE_GNUTLS=1
```

## Example

See [examples](https://github.com/paullouisageneau/libdatachannel/blob/master/examples/) for a complete usage example with signaling server (under GPLv2).

### Signal a PeerConnection

```cpp
#include "rtc/rtc.hpp"
```

```cpp
rtc::Configuration config;
config.iceServers.emplace_back("mystunserver.org:3478");

auto pc = make_shared<rtc::PeerConnection>(config);

pc->onLocalDescription([](const rtc::Description &sdp) {
    // Send the SDP to the remote peer
    MY_SEND_DESCRIPTION_TO_REMOTE(string(sdp));
});

pc->onLocalCandidate([](const rtc::Candidate &candidate) {
    // Send the candidate to the remote peer
    MY_SEND_CANDIDATE_TO_REMOTE(candidate.candidate(), candidate.mid());
});

MY_ON_RECV_DESCRIPTION_FROM_REMOTE([pc](string sdp) {
    pc->setRemoteDescription(rtc::Description(sdp));
});

MY_ON_RECV_CANDIDATE_FROM_REMOTE([pc](string candidate, string mid) {
    pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
});
```

### Observe the PeerConnection state

```cpp
pc->onStateChange([](PeerConnection::State state) {
    cout << "State: " << state << endl;
});

pc->onGatheringStateChange([](PeerConnection::GatheringState state) {
    cout << "Gathering state: " << state << endl;
});

```

### Create a DataChannel

```cpp
auto dc = pc->createDataChannel("test");
dc->onOpen([]() {
    cout << "Open" << endl;
});
dc->onMessage([](const variant<binary, string> &message) {
    if (holds_alternative<string>(message)) {
        cout << "Received: " << get<string>(message) << endl;
    }
});
```

### Receive a DataChannel

```cpp
shared_ptr<rtc::DataChannel> dc;
pc->onDataChannel([&dc](shared_ptr<rtc::DataChannel> incoming) {
    dc = incoming;
    dc->send("Hello world!");
});

```

