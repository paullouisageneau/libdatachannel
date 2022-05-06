# Example streaming to browser

This is a copy/paste example forwarding a local RTP stream to the browser.

## How to use

Open main.html in your browser.

Start the application and copy its offer into the text box on the web page.

Copy the answer of the web page back into the application.

The application expects an incoming RTP h264 video stream with payload type 96 on `localhost:6000`. On Linux, use the following gstreamer demo pipeline to capture video from a V4L2 webcam and send it as RTP to port 6000 (You might need to change `/dev/video0` to your actual device):

```
$ gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-raw,width=640,height=480 ! videoconvert ! queue ! x264enc tune=zerolatency bitrate=1000 key-int-max=30 ! video/x-h264, profile=constrained-baseline ! rtph264pay pt=96 mtu=1200 ! udpsink host=127.0.0.1 port=6000
```

