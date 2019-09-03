# libdatachannel - C/C++ WebRTC DataChannels

libdatachannel is a simple implementation of WebRTC DataChannels in C++17 with C bindings. Its API is modelled as a simplified version of the JavaScript WebRTC API, in order to ease the design of cross-environment applications.

This projet is originally inspired by [librtcdcpp](https://github.com/chadnickbok/librtcdcpp), however it is a complete rewrite from scratch, because the messy architecture of librtcdcpp made solving its implementation issues difficult.

Licensed under LGPLv2, see [LICENSE](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE).

## Dependencies

- libnice: https://github.com/libnice/libnice
- GnuTLS: https://www.gnutls.org/

Submodules:
- usrsctp: https://github.com/sctplab/usrsctp

## Building

```bash
git submodule update --init --recursive
make
```

## Example

### Signal a PeerConnection

```cpp
rtc::Configuration config;
config.iceServers.emplace_back("stunserver.org:3478");

auto pc = make_shared<rtc::PeerConnection>(config);

pc->onLocalDescription([](const rtc::Description &sdp) {
    // Send the SDP to the remote peer
    MY_SEND_DESCRIPTION_TO_REMOTE(string(sdp));
});

pc->onLocalCandidate([](const optional<rtc::Candidate> &candidate) {
    if (candidate) {
        MY_SEND_CANDIDATE_TO_REMOTE(string(*candidate), candidate->mid());
    } else {
        // Gathering finished
    }
});

MY_ON_RECV_DESCRIPTION_FROM_REMOTE([pc](string sdp) {
    pc->setRemoteDescription(rtc::Description(sdp));
});

MY_ON_RECV_CANDIDATE_FROM_REMOTE([pc](string candidate, string mid) {
    pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
});
```

### Create a DataChannel

```cpp
auto dc = pc->createDataChannel("test");
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
pc->onDataChannel([&mydc](shared_ptr<rtc::DataChannel> incoming) {
    dc = incoming;
    dc->send("Hello world!");
});

```
