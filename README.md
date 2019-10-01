# libdatachannel - C/C++ WebRTC DataChannels

libdatachannel is a standalone implementation of WebRTC DataChannels in C++17 with C bindings. It enables direct connectivity between native applications and web browsers without the pain of importing the entire WebRTC stack. Its API is modelled as a simplified version of the JavaScript WebRTC API, in order to ease the design of cross-environment applications.

This projet is originally inspired by [librtcdcpp](https://github.com/chadnickbok/librtcdcpp), however it is a complete rewrite from scratch, because the messy architecture of librtcdcpp made solving its implementation issues difficult.

Licensed under LGPLv2, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE).

## Compatibility

The library aims at fully implementing SCTP DataChannels ([draft-ietf-rtcweb-data-channel-13](https://tools.ietf.org/html/draft-ietf-rtcweb-data-channel-13)) over DTLS/UDP ([RFC7350](https://tools.ietf.org/html/rfc7350)) and has been tested to be compatible with Firefox and Chromium. It supports IPv6 and Multicast DNS candidates resolution ([draft-ietf-rtcweb-mdns-ice-candidates-03](https://tools.ietf.org/html/draft-ietf-rtcweb-mdns-ice-candidates-03)) provided the operating system also supports it.

## Dependencies

- libnice: https://github.com/libnice/libnice
- GnuTLS: https://www.gnutls.org/

Submodules:
- usrsctp: https://github.com/sctplab/usrsctp

## Building

```bash
$ git submodule update --init --recursive
$ make
```

## Example

In the following example, note the callbacks are called in another thread.

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
pc->onStateChanged([](PeerConnection::State state) {
    cout << "State: " << state << endl;
});

pc->onGatheringStateChanged([](PeerConnection::GatheringState state) {
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

See [test/main.cpp](https://github.com/paullouisageneau/libdatachannel/blob/master/test/main.cpp) for a complete local connection example.

