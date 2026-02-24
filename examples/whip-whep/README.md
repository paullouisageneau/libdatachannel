# Signaling with WHIP/WHEP using libdatachannel

This example shows how to signal using a http server

## Using the WHEP example

Generate an example video
```sh
$ gst-launch-1.0 -e \
    videotestsrc num-buffers=300 ! \
    video/x-raw,framerate=30/1 ! \
    x264enc speed-preset=ultrafast tune=zerolatency ! queue ! \
    mp4mux name=mux ! \
    filesink location=dummy_video.mp4
```

Open index.html in the browser.

Run the `whep` executable to start the http server. You should be prompted with a message saying the application is running on port 8080.

Once the http server is up, the the "Subscribe" button the begin the signaling.

When the peers sucessfully connect we can now send video the open UDP port running on port 6000.

```sh
$ gst-launch-1.0 -v \
    filesrc location=dummy_video.mp4 ! \
    qtdemux name=demux demux.video_0 ! \
    decodebin ! videoconvert ! \
    vp8enc deadline=1 target-bitrate=512000 ! \
    rtpvp8pay pt=96 ! \
    udpsink host=127.0.0.1 port=6000
```

Now you should see video on the video player in the browser

## Using the WHIP example

Open index.html in the browser.

Run the `whip` executable to start the http server. You should be prompted with a message saying the application is running on port 8080.

Once the http server is up, the the "Publish" button the begin the signaling.


After the connnection is made the libdatachannel peer will be writing the RTP packets to a UDP port running on port 5000. We now need to intercept those packets and write it to an mp4 file.

```sh
$ gst-launch-1.0 -e \
    udpsrc port=5000 caps="application/x-rtp, media=video, encoding-name=VP8-DRAFT-IETF-01, payload=96" ! \
    rtpvp8depay ! webmmux ! filesink location=output.webm
```
