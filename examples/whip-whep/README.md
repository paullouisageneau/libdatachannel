# Signaling with WHIP/WHEP using libdatachannel

This example signals over HTTP following
[RFC 9725](https://www.rfc-editor.org/rfc/rfc9725.html) (WHIP) and the WHEP
draft. A single server hosts the example page and both endpoints, so publishing
and subscribing can be tested against each other with no other tooling.

Only VP8 is handled. The server removes every other codec from its answers, so a
peer cannot fall back to a format the example is unable to forward.

## Running it

```sh
$ ./whip-whep
Open http://localhost:8080 in your browser
RTP in: udp/6000   RTP out: udp/5000
```

Open the page, press **Publish** to send your camera to the server over WHIP,
then press **Subscribe** to pull the same stream back over WHEP. The video
appearing on the right has made a full round trip through libdatachannel.

Pass a port to move the server out of the way of anything already using 8080,
and optionally a directory to serve the page from:

```sh
$ ./whip-whep 9090
$ ./whip-whep 9090 /path/to/web/root
```

## Endpoints

| Method | Path | Meaning |
| --- | --- | --- |
| `GET` | `/` | the page in `index.html` |
| `POST` | `/whip` | publish: send an SDP offer, receive `201 Created` with the answer and a `Location` header |
| `POST` | `/whep` | subscribe: same, in the other direction |
| `DELETE` | the `Location` path | tear that session down, `200 OK` |
| `OPTIONS` | any of the above | CORS preflight |

One publisher and one subscriber are accepted at a time; a second offer for a
role that is already taken is answered with `409 Conflict`. The **Stop** buttons
issue the `DELETE`.

## RTP routing

Every source is forwarded to every sink, which is what lets the same build serve
the browser-to-browser, record, and playback cases:

```
     WHIP peer  ─┐                 ┌─►  WHEP peer
                 ├──►  server  ────┤
  udp/6000 in  ─┘                 └─►  udp/5000 out
```

### Recording what a browser publishes

Start the receiver before pressing **Publish**, since nothing is buffered:

```sh
$ gst-launch-1.0 -e \
    udpsrc port=5000 caps="application/x-rtp, media=video, encoding-name=VP8-DRAFT-IETF-01, payload=96" ! \
    rtpvp8depay ! webmmux ! filesink location=output.webm
```

Stop it with `Ctrl-C` so the `-e` flag can send EOS and finalize the file.

### Playing video out to a browser

Press **Subscribe** in the browser first, since nothing is buffered, then send
RTP to `udp/6000`. The simplest source is a live test pattern:

```sh
$ gst-launch-1.0 -v \
    videotestsrc is-live=true ! \
    video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! \
    vp8enc deadline=1 target-bitrate=512000 ! \
    rtpvp8pay pt=96 ! \
    udpsink host=127.0.0.1 port=6000
```

To play a file instead, generate one first. Keep it VP8: the example is VP8
throughout, and encoding the intermediate clip as H.264 only adds a decoder
dependency that many GStreamer installations do not ship.

```sh
$ gst-launch-1.0 -e \
    videotestsrc num-buffers=300 ! \
    video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! \
    vp8enc deadline=1 ! \
    webmmux ! \
    filesink location=dummy_video.webm
```

```sh
$ gst-launch-1.0 -v \
    filesrc location=dummy_video.webm ! \
    matroskademux ! vp8dec ! videoconvert ! \
    vp8enc deadline=1 target-bitrate=512000 ! \
    rtpvp8pay pt=96 ! \
    udpsink host=127.0.0.1 port=6000
```

The explicit `format=I420` matters. Left unconstrained, `videotestsrc` can
negotiate a high-bit-depth format, and the encoder will happily produce a
profile that no installed decoder can read back.
