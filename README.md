# libdatachannel - C/C++ WebRTC network library

[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-blue.svg)](https://www.mozilla.org/en-US/MPL/2.0/)
[![Build with GnuTLS](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-gnutls.yml/badge.svg)](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-gnutls.yml)
[![Build with Mbed TLS](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-mbedtls.yml/badge.svg)](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-mbedtls.yml)
[![Build with OpenSSL](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-openssl.yml/badge.svg)](https://github.com/paullouisageneau/libdatachannel/actions/workflows/build-openssl.yml)

[![AUR package](https://repology.org/badge/version-for-repo/aur/libdatachannel.svg)](https://repology.org/project/libdatachannel/versions) [![FreeBSD port](https://repology.org/badge/version-for-repo/freebsd/libdatachannel.svg)](https://repology.org/project/libdatachannel/versions) [![Vcpkg package](https://repology.org/badge/version-for-repo/vcpkg/libdatachannel.svg)](https://repology.org/project/libdatachannel/versions)
[![Gitter](https://badges.gitter.im/libdatachannel/community.svg)](https://gitter.im/libdatachannel/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)
[![Discord](https://img.shields.io/discord/903257095539925006?logo=discord)](https://discord.gg/jXAP8jp3Nn)

libdatachannel is a standalone implementation of WebRTC Data Channels, WebRTC Media Transport, and WebSockets in C++17 with C bindings for POSIX platforms (including GNU/Linux, Android, FreeBSD, Apple macOS and iOS) and Microsoft Windows. WebRTC is a W3C and IETF standard enabling real-time peer-to-peer data and media exchange between two devices.

The library aims at being both straightforward and lightweight with minimal external dependencies, to enable direct connectivity between native applications and web browsers without the pain of importing Google's bloated [reference library](https://webrtc.googlesource.com/src/). The interface consists of somewhat simplified versions of the JavaScript WebRTC and WebSocket APIs present in browsers, in order to ease the design of cross-environment applications.

It can be compiled with multiple backends:
- The security layer can be provided through [GnuTLS](https://www.gnutls.org/), [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/), or [OpenSSL](https://www.openssl.org/).
- The connectivity for WebRTC can be provided through my ad-hoc ICE library [libjuice](https://github.com/paullouisageneau/libjuice) as submodule or through [libnice](https://github.com/libnice/libnice).

The WebRTC stack is fully compatible with browsers like Firefox and Chromium, see [Compatibility](#Compatibility) below. Additionally, code using Data Channels and WebSockets from the library may be compiled as is to WebAssembly for browsers with [datachannel-wasm](https://github.com/paullouisageneau/datachannel-wasm).

libdatachannel is licensed under MPL 2.0 since version 0.18, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE) (previous versions were licensed under LGPLv2.1 or later).

libdatachannel is available on [AUR](https://aur.archlinux.org/packages/libdatachannel/), [vcpkg](https://vcpkg.io/en/getting-started), and [FreeBSD ports](https://www.freshports.org/www/libdatachannel). Bindings are available for [Rust](https://crates.io/crates/datachannel) and [Node.js](https://www.npmjs.com/package/node-datachannel).

## Dependencies

- [GnuTLS](https://www.gnutls.org/), [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/), or [OpenSSL](https://www.openssl.org/)
- [usrsctp](https://github.com/sctplab/usrsctp) (as submodule by default)
- [Plog](https://github.com/SergiusTheBest/plog) (as submodule by default)
- [libjuice](https://github.com/paullouisageneau/libjuice) (as submodule by default) or [libnice](https://nice.freedesktop.org/) as an ICE backend.
- [libsrtp](https://github.com/cisco/libsrtp) (as submodule by default) required if compiled with media support.
- [nlohmann JSON](https://github.com/nlohmann/json) (as submodule by default) required to build examples.

## Building

See [BUILDING.md](https://github.com/paullouisageneau/libdatachannel/blob/master/BUILDING.md) for building instructions.

## Examples

See [examples](https://github.com/paullouisageneau/libdatachannel/blob/master/examples/) for complete usage examples with signaling server (under MPL 2.0).

Additionally, you might want to have a look at the [C API documentation](https://github.com/paullouisageneau/libdatachannel/blob/master/DOC.md).

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

WebRTC allows real-time data and media exchange between two devices through a Peer Connection (or RTCPeerConnection), a signaled peer-to-peer connection which can carry both Data Channels and media tracks. It is compatible with browsers Firefox, Chromium, and Safari, and other WebRTC libraries (see [webrtc-echoes](https://github.com/sipsorcery/webrtc-echoes)). Media transport is optional and can be disabled at compile time.

Protocol stack:
- SCTP-based Data Channels ([RFC8831](https://www.rfc-editor.org/rfc/rfc8831.html))
- SRTP-based Media Transport ([RFC8834](https://www.rfc-editor.org/rfc/rfc8834.html))
- DTLS/UDP ([RFC7350](https://www.rfc-editor.org/rfc/rfc7350.html) and [RFC8261](https://www.rfc-editor.org/rfc/rfc8261.html))
- ICE ([RFC8445](https://www.rfc-editor.org/rfc/rfc8445.html)) with STUN ([RFC8489](https://www.rfc-editor.org/rfc/rfc8489.html)) and its extension TURN ([RFC8656](https://www.rfc-editor.org/rfc/rfc8656.html))

Features:
- Full IPv6 support (as mandated by [RFC8835](https://www.rfc-editor.org/rfc/rfc8835.html))
- Trickle ICE ([RFC8838](https://www.rfc-editor.org/rfc/rfc8838.html))
- JSEP-compatible session establishment with SDP ([RFC8829](https://www.rfc-editor.org/rfc/rfc8829.html))
- SCTP over DTLS with SDP offer/answer ([RFC8841](https://www.rfc-editor.org/rfc/rfc8841.html))
- DTLS with ECDSA or RSA keys ([RFC8827](https://www.rfc-editor.org/rfc/rfc8827.html))
- SRTP and SRTCP key derivation from DTLS ([RFC5764](https://www.rfc-editor.org/rfc/rfc5764.html))
- Differentiated Services QoS ([RFC8837](https://www.rfc-editor.org/rfc/rfc8837.html)) where possible
- Multicast DNS candidates ([draft-ietf-rtcweb-mdns-ice-candidates-04](https://datatracker.ietf.org/doc/html/draft-ietf-rtcweb-mdns-ice-candidates-04))
- Multiplexing connections on a single UDP port with libjuice as ICE backend

Note only SDP BUNDLE mode is supported for media multiplexing ([RFC8843](https://www.rfc-editor.org/rfc/rfc8843.html)). The behavior is equivalent to the JSEP bundle-only policy: the library always negotiates one unique network component, where SRTP media streams are multiplexed with SRTCP control packets ([RFC5761](https://www.rfc-editor.org/rfc/rfc5761.html)) and SCTP/DTLS data traffic ([RFC8261](https://www.rfc-editor.org/rfc/rfc8261.html)).

### WebSocket

WebSocket is the protocol of choice for WebRTC signaling. The support is optional and can be disabled at compile time.

Protocol stack:
- WebSocket protocol ([RFC6455](https://www.rfc-editor.org/rfc/rfc6455.html)), client and server side
- HTTP over TLS ([RFC2818](https://www.rfc-editor.org/rfc/rfc2818.html))

Features:
- IPv6 and IPv4/IPv6 dual-stack support
- Keepalive with ping/pong

## External resources
- Rust bindings for libdatachannel: [datachannel-rs](https://github.com/lerouxrgd/datachannel-rs)
- Node.js bindings for libdatachannel: [node-datachannel](https://github.com/murat-dogan/node-datachannel)
- Unity bindings for Windows 10 and Hololens: [datachannel-unity](https://github.com/hanseuljun/datachannel-unity)
- WebAssembly wrapper compatible with libdatachannel: [datachannel-wasm](https://github.com/paullouisageneau/datachannel-wasm)
- Lightweight STUN/TURN server: [Violet](https://github.com/paullouisageneau/violet)
- Native platform (Android/iOS/macOS) wrapper for libdatachannel: [datachannel-native](https://github.com/swarm-cloud/datachannel-native)

## Thanks

Thanks to [Streamr](https://streamr.network/), [Vagon](https://vagon.io/), [Shiguredo](https://github.com/shiguredo), [Deon Botha](https://github.com/dbotha), and [Michael Cho](https://github.com/micoolcho) for sponsoring this work!

