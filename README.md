# libdatachannel - C/C++ WebRTC Data Channels

libdatachannel is a standalone implementation of WebRTC Data Channels and WebSockets in C++17 with C bindings for POSIX platforms (including Linux and Apple macOS) and Microsoft Windows. It enables direct connectivity between native applications and web browsers without the pain of importing the entire WebRTC stack. The interface consists of simplified versions of the JavaScript WebRTC and WebSocket APIs present in browsers, in order to ease the design of cross-environment applications.
It can be compiled with multiple backends:
- The security layer can be provided through [GnuTLS](https://www.gnutls.org/) or [OpenSSL](https://www.openssl.org/).
- The connectivity for WebRTC can be provided through my ad-hoc ICE library [libjuice](https://github.com/paullouisageneau/libjuice) as submodule or through [libnice](https://github.com/libnice/libnice).

This projet is originally inspired by [librtcdcpp](https://github.com/chadnickbok/librtcdcpp), however it is a complete rewrite from scratch, because the messy architecture of librtcdcpp made solving its implementation issues difficult.

Licensed under LGPLv2, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE).

## Compatibility

The library aims at implementing the following communication protocols:

### WebRTC Data Channel

The WebRTC stack has been tested to be compatible with Firefox and Chromium.

Protocol stack:
- SCTP-based Data Channels ([draft-ietf-rtcweb-data-channel-13](https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13))
- DTLS/UDP ([RFC7350](https://tools.ietf.org/html/rfc7350) and [RFC8261](https://tools.ietf.org/html/rfc8261))
- ICE ([RFC8445](https://tools.ietf.org/html/rfc8445)) with STUN ([RFC5389](https://tools.ietf.org/html/rfc5389))

Features:
- Full IPv6 support
- Trickle ICE ([draft-ietf-ice-trickle-21](https://tools.ietf.org/html/draft-ietf-ice-trickle-21))
- JSEP compatible ([draft-ietf-rtcweb-jsep-26](https://tools.ietf.org/html/draft-ietf-rtcweb-jsep-26))
- Multicast DNS candidates ([draft-ietf-rtcweb-mdns-ice-candidates-04](https://tools.ietf.org/html/draft-ietf-rtcweb-mdns-ice-candidates-04))
- TURN relaying ([RFC5766](https://tools.ietf.org/html/rfc5766)) with [libnice](https://github.com/libnice/libnice) as ICE backend
- SRTP media transport ([RFC3711](https://tools.ietf.org/html/rfc3711)) with [libSRTP](https://github.com/cisco/libsrtp)

### WebSocket

WebSocket is the protocol of choice for WebRTC signaling. The support is optional and can be disabled at compile time.

Protocol stack:
- WebSocket protocol ([RFC6455](https://tools.ietf.org/html/rfc6455)), client-side only
- HTTP over TLS ([RFC2818](https://tools.ietf.org/html/rfc2818))

Features:
- IPv6 and IPv4/IPv6 dual-stack support
- Keepalive with ping/pong

## Dependencies

Dependencies:
- GnuTLS: https://www.gnutls.org/ or OpenSSL: https://www.openssl.org/

Optional dependencies:
- libnice: https://nice.freedesktop.org/ (substituable with libjuice)
- libSRTP: https://github.com/cisco/libsrtp (only necessary for media transport)

Submodules:
- libjuice: https://github.com/paullouisageneau/libjuice
- usrsctp: https://github.com/sctplab/usrsctp

## Building

### Clone repository and submodules

```bash
$ git clone https://github.com/paullouisageneau/libdatachannel.git
$ cd libdatachannel
$ git submodule update --init --recursive
```

### Building with CMake

The CMake library targets `libdatachannel` and `libdatachannel-static` respectively correspond to the shared and static libraries. On Windows, the DLL resulting from the shared library build only exposes the C API, use the static library for the C++ API. The default target will build tests and examples.

#### POSIX-compliant operating systems (including Linux and Apple macOS)
```bash
$ cmake -B build -DUSE_JUICE=1 -DUSE_GNUTLS=1
$ cd build && make -j2
```

#### Microsoft Windows with MinGW cross-compilation
```bash
$ cmake -B build -DUSE_JUICE=1 -DCMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-x86_64-w64-mingw32.cmake # replace with your toolchain file
$ cd build && make -j2
```

#### Microsoft Windows with Microsoft Visual C++
```bash
$ cmake -B build -G "NMake Makefiles" -DUSE_JUICE=1
$ cd build
$ nmake
```

### Building directly with Make (Linux only)

```bash
$ make USE_JUICE=1 USE_GNUTLS=1
```

## Examples

See [examples](https://github.com/paullouisageneau/libdatachannel/blob/master/examples/) for a complete usage example with signaling server (under GPLv2).

Additionnaly, you might want to have a look at the [C API](https://github.com/paullouisageneau/libdatachannel/blob/dev/include/rtc/rtc.h).

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

### Open a WebSocket

```cpp
auto ws = make_shared<rtc::WebSocket>();

ws->onOpen([]() {
	cout << "WebSocket open" << endl;
});

ws->onMessage([](const variant<binary, string> &message) {
    if (holds_alternative<string>(message)) {
        cout << "WebSocket received: " << get<string>(message) << endl;
    }
});

ws->open("wss://my.websocket/service");

```

