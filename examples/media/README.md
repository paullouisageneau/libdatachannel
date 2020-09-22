# Example Webcam from Browser to Port 5000
This is an example copy/paste demo to send your webcam from your browser and out port 5000 through the demo application.

## How to use
Open main.html in your browser (you must open it either as HTTPS or as a domain of http://localhost).

Start the application and copy it's offer into the text box of the web page.

Copy the answer of the webpage back into the application.

You will now see RTP traffic on `localhost:5000` of the computer that the application is running on.

Use the following gstreamer demo pipeline to display the traffic
(you might need to wave your hand in front of your camera to force an I-frame).

```
$ gst-launch-1.0 udpsrc address=127.0.0.1 port=5000 caps="application/x-rtp" ! queue ! rtph264depay ! video/x-h264,stream-format=byte-stream ! queue ! avdec_h264 ! queue ! autovideosink
```

