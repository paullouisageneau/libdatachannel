# libdatachannel - C/C++ WebRTC Data Channels

libdatachannel is a standalone implementation of WebRTC Data Channels, WebRTC Media Transport, and WebSockets in C++17 with C bindings for POSIX platforms (including GNU/Linux, Android, and Apple macOS) and Microsoft Windows. It enables direct connectivity between native applications and web browsers without the pain of importing the entire WebRTC stack. The interface consists of simplified versions of the JavaScript WebRTC and WebSocket APIs present in browsers, in order to ease the design of cross-environment applications.
It can be compiled with multiple backends:
- The security layer can be provided through [OpenSSL](https://www.openssl.org/) or [GnuTLS](https://www.gnutls.org/).
- The connectivity for WebRTC can be provided through my ad-hoc ICE library [libjuice](https://github.com/paullouisageneau/libjuice) as submodule or through [libnice](https://github.com/libnice/libnice).

Licensed under LGPLv2, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE).

## Compatibility

The library implements the following communication protocols:

### WebRTC Data Channels and Media Transport

The WebRTC stack has been tested to be compatible with Firefox and Chromium.

Protocol stack:
- SCTP-based Data Channels ([draft-ietf-rtcweb-data-channel-13](https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13))
- SRTP-based Media Transport ([draft-ietf-rtcweb-rtp-usage-26](https://tools.ietf.org/html/draft-ietf-rtcweb-rtp-usage-26))
- DTLS/UDP ([RFC7350](https://tools.ietf.org/html/rfc7350) and [RFC8261](https://tools.ietf.org/html/rfc8261))
- ICE ([RFC8445](https://tools.ietf.org/html/rfc8445)) with STUN ([RFC8489](https://tools.ietf.org/html/rfc8489)) and its extension TURN ([RFC8656](https://tools.ietf.org/html/rfc8656))

Features:
- Full IPv6 support
- Trickle ICE ([draft-ietf-ice-trickle-21](https://tools.ietf.org/html/draft-ietf-ice-trickle-21))
- JSEP compatible ([draft-ietf-rtcweb-jsep-26](https://tools.ietf.org/html/draft-ietf-rtcweb-jsep-26))
- Multicast DNS candidates ([draft-ietf-rtcweb-mdns-ice-candidates-04](https://tools.ietf.org/html/draft-ietf-rtcweb-mdns-ice-candidates-04))
- SRTP and SRTCP key derivation from DTLS ([RFC5764](https://tools.ietf.org/html/rfc5764))
- Differentiated Services QoS ([draft-ietf-tsvwg-rtcweb-qos-18](https://tools.ietf.org/html/draft-ietf-tsvwg-rtcweb-qos-18))

Note only SDP BUNDLE mode is supported for media multiplexing ([draft-ietf-mmusic-sdp-bundle-negotiation-54](https://tools.ietf.org/html/draft-ietf-mmusic-sdp-bundle-negotiation-54)). The behavior is equivalent to the JSEP bundle-only policy: the library always negociates one unique network component, where SRTP media streams are multiplexed with SRTCP control packets ([RFC5761](https://tools.ietf.org/html/rfc5761)) and SCTP/DTLS data traffic ([RFC5764](https://tools.ietf.org/html/rfc5764)).

### WebSocket

WebSocket is the protocol of choice for WebRTC signaling. The support is optional and can be disabled at compile time.

Protocol stack:
- WebSocket protocol ([RFC6455](https://tools.ietf.org/html/rfc6455)), client-side only
- HTTP over TLS ([RFC2818](https://tools.ietf.org/html/rfc2818))

Features:
- IPv6 and IPv4/IPv6 dual-stack support
- Keepalive with ping/pong

## Dependencies

Only [GnuTLS](https://www.gnutls.org/) or [OpenSSL](https://www.openssl.org/) are necessary.

Optionally, [libnice](https://nice.freedesktop.org/) can be selected as an alternative ICE backend instead of libjuice.

Submodules:
- libjuice: https://github.com/paullouisageneau/libjuice
- usrsctp: https://github.com/sctplab/usrsctp
- libsrtp: https://github.com/cisco/libsrtp

## Building

See [BUILDING.md](https://github.com/paullouisageneau/libdatachannel/blob/master/BUILDING.md) for building instructions.

## Examples

See [examples](https://github.com/paullouisageneau/libdatachannel/blob/master/examples/) for complete usage examples with signaling server (under GPLv2).

Additionnaly, you might want to have a look at the [C API](https://github.com/paullouisageneau/libdatachannel/blob/master/include/rtc/rtc.h).

### Signal a PeerConnection

```cpp
#include "rtc/rtc.hpp"
```

```cpp
rtc::Configuration config;
config.iceServers.emplace_back("mystunserver.org:3478");

rtc::PeerConection pc(config);

pc.onLocalDescription([](rtc::Description sdp) {
    // Send the SDP to the remote peer
    MY_SEND_DESCRIPTION_TO_REMOTE(string(sdp));
});

pc.onLocalCandidate([](rtc::Candidate candidate) {
    // Send the candidate to the remote peer
    MY_SEND_CANDIDATE_TO_REMOTE(candidate.candidate(), candidate.mid());
});

MY_ON_RECV_DESCRIPTION_FROM_REMOTE([&pc](string sdp) {
    pc.setRemoteDescription(rtc::Description(sdp));
});

MY_ON_RECV_CANDIDATE_FROM_REMOTE([&pc](string candidate, string mid) {
    pc.addRemoteCandidate(rtc::Candidate(candidate, mid));
});
```

### Observe the PeerConnection state

```cpp
pc.onStateChange([](PeerConnection::State state) {
    cout << "State: " << state << endl;
});

pc.onGatheringStateChange([](PeerConnection::GatheringState state) {
    cout << "Gathering state: " << state << endl;
});
```

### Create a DataChannel

```cpp
auto dc = pc.createDataChannel("test");

dc->onOpen([]() {
    cout << "Open" << endl;
});

dc->onMessage([](variant<binary, string> message) {
    if (holds_alternative<string>(message)) {
        cout << "Received: " << get<string>(message) << endl;
    }
});
```

### Receive a DataChannel

```cpp
shared_ptr<rtc::DataChannel> dc;
pc.onDataChannel([&dc](shared_ptr<rtc::DataChannel> incoming) {
    dc = incoming;
    dc->send("Hello world!");
});
```

### Open a WebSocket

```cpp
rtc::WebSocket ws;

ws.onOpen([]() {
	cout << "WebSocket open" << endl;
});

ws.onMessage([](variant<binary, string> message) {
    if (holds_alternative<string>(message)) {
        cout << "WebSocket received: " << get<string>(message) << endl;
    }
});

ws.open("wss://my.websocket/service");
```

## External resources
- Rust wrapper for libdatachannel: [datachannel-rs](https://github.com/lerouxrgd/datachannel-rs)
- Node.js wrapper for libdatachannel: [node-datachannel](https://github.com/murat-dogan/node-datachannel)
- Unity wrapper for Windows 10 and Hololens: [datachannel-unity](https://github.com/hanseuljun/datachannel-unity)
- WebAssembly wrapper compatible with libdatachannel: [datachannel-wasm](https://github.com/paullouisageneau/datachannel-wasm)

## Thanks

Thanks to [Streamr](https://streamr.network/) for sponsoring this work!

