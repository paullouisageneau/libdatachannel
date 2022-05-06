# libdatachannel - Examples

This directory contains different WebRTC clients and compatible WebSocket + JSON signaling servers.

- [client](client) contains a native client to open Data Channels with WebSocket signaling using libdatachannel
- [client-benchmark](client-benchmark)  contains a native client to open Data Channels with WebSocket signaling using libdatachannel and benchmark functionalities. See [client-benchmark/README.md](client-benchmark/README.md) for usage examples.
- [web](web) contains a equivalent JavaScript client for web browsers
- [signaling-server-nodejs](signaling-server-nodejs) contains a signaling server in node.js
- [signaling-server-python](signaling-server-python) contains a similar signaling server in Python
- [signaling-server-rust](signaling-server-rust) contains a similar signaling server in Rust (see [lerouxrgd/datachannel-rs](https://github.com/lerouxrgd/datachannel-rs) for Rust wrappers)

- [media-receiver](media-receiver) is a copy/paste example sending the webcam from the web browser and receiving it into gstreamer.
- [media-sender](media-sender) is a copy/paste example capturing the webcam with gstreamer and sending it to the web browser.
- [media-sfu](media-sfu) is a copy/paste SFU relaying the webcam between web browsers.
- [streamer](streamer) streams h264 and opus samples to web browsers (signaling-server-python is required).

Additionally, it contains two debugging tools for libdatachannel with copy-pasting as signaling:
- [copy-paste](copy-paste) using the C++ API
- [copy-paste-capi](copy-paste-capi) using the C API

