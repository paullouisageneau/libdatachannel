# Streaming H264 and opus

This example streams H264 and opus<sup id="a1">[1](#f1)</sup> samples to the connected browser client.

## Start the example signaling server

```sh
$ python3 examples/signaling-server-python/signaling-server.py
```

## Start a web server

```sh
$ cd examples/streamer
$ python3 -m http.server --bind 127.0.0.1 8080
```

## Start the streamer

```sh
$ cd build/examples/streamer
$ ./streamer
```
Arguments:

- `-a` Directory with OPUS samples (default: *../../../../examples/streamer/samples/opus/*).
- `-b` Directory with H264 samples (default: *../../../../examples/streamer/samples/h264/*).
- `-d` Signaling server IP address (default: 127.0.0.1).
- `-p` Signaling server port (default: 8000).
- `-v` Enable debug logs.
- `-h` Print this help and exit.

You can now open the example at the web server URL [http://127.0.0.1:8080](http://127.0.0.1:8080).

## Generating H264 and Opus samples

You can generate H264 and Opus sample with *samples/generate_h264.py* and *samples/generate_opus.py* respectively. This require ffmpeg, python3 and kaitaistruct library to be installed. Use `-h`/`--help` to learn more about arguments.

<b id="f1">1</b> Opus samples are generated from music downloaded at [bensound](https://www.bensound.com). [↩](#a1)
