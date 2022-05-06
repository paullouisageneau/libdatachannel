# Example streaming from browser

This is a copy/paste example streaming the webcam from the browser and forwarding it as RTP to port 5000.

## How to use

Open main.html in your browser (You must open it either as HTTPS or as a domain of http://localhost).

Start the application and copy its offer into the text box on the web page.

Copy the answer of the web page back into the application.

You will now see RTP traffic on `localhost:5000` of the computer that the application is running on.

Use the following gstreamer demo pipeline to display the traffic (You might need to wave your hand in front of your camera to force an I-frame):

```
$ gst-launch-1.0 udpsrc address=127.0.0.1 port=5000 caps="application/x-rtp" ! queue ! rtph264depay ! video/x-h264,stream-format=byte-stream ! queue ! avdec_h264 ! queue ! autovideosink
```

