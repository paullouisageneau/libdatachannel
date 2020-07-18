# Examples for libdatachannel

This directory contains different WebRTC clients and compatible WebSocket + JSON signaling servers.

- [client](client) uses libdatachannel to implement WebRTC Data Channels with WebSocket signaling
- [web](web) contains an equivalent implementation for web browsers and a node.js signaling server
- [signaling-server-python](signaling-server-python) contains a similar signaling server in Python
- [signaling-server-rust](signaling-server-rust) contains a similar signaling server in Rust (see [lerouxrgd/datachannel-rs](https://github.com/lerouxrgd/datachannel-rs) for Rust wrappers)

Additionally, it contains two debugging tools for libdatachannel with copy-pasting as signaling:
- [copy-paste](copy-paste) using the C++ API
- [copy-paste-capi](copy-paste-capi) using the C API

