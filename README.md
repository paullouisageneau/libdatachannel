# libdatachannel - C/C++ WebRTC network library

[![Build with OpenSSL](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-openssl.yml/badge.svg)](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-openssl.yml)
[![Build with GnuTLS](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-gnutls.yml/badge.svg)](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-gnutls.yml)
[![Join the chat at https://gitter.im/libdatachannel/community](https://badges.gitter.im/libdatachannel/community.svg)](https://gitter.im/libdatachannel/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)
[![Discord](https://img.shields.io/discord/903257095539925006?logo=discord)](https://discord.gg/jXAP8jp3Nn)

[![AUR package](https://repology.org/badge/version-for-repo/aur/libdatachannel.svg)](https://repology.org/project/libdatachannel/versions) [![FreeBSD port](https://repology.org/badge/version-for-repo/freebsd/libdatachannel.svg)](https://repology.org/project/libdatachannel/versions) [![Vcpkg package](https://repology.org/badge/version-for-repo/vcpkg/libdatachannel.svg)](https://repology.org/project/libdatachannel/versions)

libdatachannel is a standalone implementation of WebRTC Data Channels, WebRTC Media Transport, and WebSockets in C++17 with C bindings for POSIX platforms (including GNU/Linux, Android, FreeBSD, Apple macOS and iOS) and Microsoft Windows.

The library aims at being both straightforward and lightweight with minimal external dependencies, to enable direct connectivity between native applications and web browsers without the pain of importing Google's bloated [reference library](https://webrtc.googlesource.com/src/). The interface consists of somewhat simplified versions of the JavaScript WebRTC and WebSocket APIs present in browsers, in order to ease the design of cross-environment applications.

It can be compiled with multiple backends:
- The security layer can be provided through [OpenSSL](https://www.openssl.org/) or [GnuTLS](https://www.gnutls.org/).
- The connectivity for WebRTC can be provided through my ad-hoc ICE library [libjuice](https://github.com/paullouisageneau/libjuice) as submodule or through [libnice](https://github.com/libnice/libnice).

The WebRTC stack is fully compatible with browsers like Firefox and Chromium, see [Compatibility](#Compatibility) below. Additionally, code using Data Channels and WebSockets from the library may be compiled as is to WebAssembly for browsers with [datachannel-wasm](https://github.com/paullouisageneau/datachannel-wasm).

libdatachannel is licensed under LGPLv2, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE).

libdatachannel is available on [AUR](https://aur.archlinux.org/packages/libdatachannel/) and [vcpkg](https://vcpkg.info/port/libdatachannel).

## Dependencies

Only [GnuTLS](https://www.gnutls.org/) or [OpenSSL](https://www.openssl.org/) are necessary. Optionally, [libnice](https://nice.freedesktop.org/) can be selected as an alternative ICE backend instead of libjuice.

Submodules:
- usrsctp: https://github.com/sctplab/usrsctp
- plog: https://github.com/SergiusTheBest/plog
- libjuice: https://github.com/paullouisageneau/libjuice (if not compiled with libnice backend)
- libsrtp: https://github.com/cisco/libsrtp (if compiled with media support)

## Building

See [BUILDING.md](https://github.com/paullouisageneau/libdatachannel/blob/master/BUILDING.md) for building instructions.

## Examples

See [examples](https://github.com/paullouisageneau/libdatachannel/blob/master/examples/) for complete usage examples with signaling server (under GPLv2).

Additionnaly, you might want to have a look at the [C API documentation](https://github.com/paullouisageneau/libdatachannel/blob/master/DOC.md).

### Signal a PeerConnection

```cpp
#include "rtc/rtc.hpp"
```

```cpp
rtc::Configuration config;
config.iceServers.emplace_back("mystunserver.org:3478");

rtc::PeerConnection pc(config);

pc.onLocalDescription([](rtc::Description sdp) {
    // Send the SDP to the remote peer
    MY_SEND_DESCRIPTION_TO_REMOTE(std::string(sdp));
});

pc.onLocalCandidate([](rtc::Candidate candidate) {
    // Send the candidate to the remote peer
    MY_SEND_CANDIDATE_TO_REMOTE(candidate.candidate(), candidate.mid());
});

MY_ON_RECV_DESCRIPTION_FROM_REMOTE([&pc](std::string sdp) {
    pc.setRemoteDescription(rtc::Description(sdp));
});

MY_ON_RECV_CANDIDATE_FROM_REMOTE([&pc](std::string candidate, std::string mid) {
    pc.addRemoteCandidate(rtc::Candidate(candidate, mid));
});
```

### Observe the PeerConnection state

```cpp
pc.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "State: " << state << std::endl;
});

pc.onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "Gathering state: " << state << std::endl;
});
```

### Create a DataChannel

```cpp
auto dc = pc.createDataChannel("test");

dc->onOpen([]() {
    std::cout << "Open" << std::endl;
});

dc->onMessage([](std::variant<rtc::binary, rtc::string> message) {
    if (std::holds_alternative<rtc::string>(message)) {
        std::cout << "Received: " << get<rtc::string>(message) << std::endl;
    }
});
```

### Receive a DataChannel

```cpp
std::shared_ptr<rtc::DataChannel> dc;
pc.onDataChannel([&dc](std::shared_ptr<rtc::DataChannel> incoming) {
    dc = incoming;
    dc->send("Hello world!");
});
```

### Open a WebSocket

```cpp
rtc::WebSocket ws;

ws.onOpen([]() {
    std::cout << "WebSocket open" << std::endl;
});

ws.onMessage([](std::variant<rtc::binary, rtc::string> message) {
    if (std::holds_alternative<rtc::string>(message)) {
        std::cout << "WebSocket received: " << std::get<rtc::string>(message) << endl;
    }
});

ws.open("wss://my.websocket/service");
```

## Compatibility

The library implements the following communication protocols:

### WebRTC Data Channels and Media Transport

The library implements WebRTC Peer Connections with both Data Channels and Media Transport. Media transport is optional and can be disabled at compile time.

Protocol stack:
- SCTP-based Data Channels ([RFC8831](https://tools.ietf.org/html/rfc8831))
- SRTP-based Media Transport ([RFC8834](https://tools.ietf.org/html/rfc8834))
- DTLS/UDP ([RFC7350](https://tools.ietf.org/html/rfc7350) and [RFC8261](https://tools.ietf.org/html/rfc8261))
- ICE ([RFC8445](https://tools.ietf.org/html/rfc8445)) with STUN ([RFC8489](https://tools.ietf.org/html/rfc8489)) and its extension TURN ([RFC8656](https://tools.ietf.org/html/rfc8656))

Features:
- Full IPv6 support (as mandated by [RFC8835](https://tools.ietf.org/html/rfc8835))
- Trickle ICE ([RFC8838](https://tools.ietf.org/html/rfc8838))
- JSEP-compatible session establishment with SDP ([RFC8829](https://tools.ietf.org/html/rfc8829))
- SCTP over DTLS with SDP offer/answer ([RFC8841](https://tools.ietf.org/html/rfc8841))
- DTLS with ECDSA or RSA keys ([RFC8824](https://tools.ietf.org/html/rfc8827))
- SRTP and SRTCP key derivation from DTLS ([RFC5764](https://tools.ietf.org/html/rfc5764))
- Differentiated Services QoS ([RFC8837](https://tools.ietf.org/html/rfc8837)) where possible
- Multicast DNS candidates ([draft-ietf-rtcweb-mdns-ice-candidates-04](https://tools.ietf.org/html/draft-ietf-rtcweb-mdns-ice-candidates-04))

Note only SDP BUNDLE mode is supported for media multiplexing ([RFC8843](https://tools.ietf.org/html/rfc8843)). The behavior is equivalent to the JSEP bundle-only policy: the library always negociates one unique network component, where SRTP media streams are multiplexed with SRTCP control packets ([RFC5761](https://tools.ietf.org/html/rfc5761)) and SCTP/DTLS data traffic ([RFC8261](https://tools.ietf.org/html/rfc8261)).

### WebSocket

WebSocket is the protocol of choice for WebRTC signaling. The support is optional and can be disabled at compile time.

Protocol stack:
- WebSocket protocol ([RFC6455](https://tools.ietf.org/html/rfc6455)), client and server side
- HTTP over TLS ([RFC2818](https://tools.ietf.org/html/rfc2818))

Features:
- IPv6 and IPv4/IPv6 dual-stack support
- Keepalive with ping/pong

## External resources
- Rust wrapper for libdatachannel: [datachannel-rs](https://github.com/lerouxrgd/datachannel-rs)
- Node.js wrapper for libdatachannel: [node-datachannel](https://github.com/murat-dogan/node-datachannel)
- Unity wrapper for Windows 10 and Hololens: [datachannel-unity](https://github.com/hanseuljun/datachannel-unity)
- WebAssembly wrapper compatible with libdatachannel: [datachannel-wasm](https://github.com/paullouisageneau/datachannel-wasm)
- Lightweight STUN/TURN server: [Violet](https://github.com/paullouisageneau/violet)

## Thanks

Thanks to [Streamr](https://streamr.network/) for sponsoring this work!

