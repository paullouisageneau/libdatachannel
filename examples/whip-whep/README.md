# Signaling with WHIP/WHEP using libdatachannel

This example shows how to signal using a http server

## Using the WHEP example

Generate an example video
```sh
$ ffmpeg -f lavfi -i testsrc=duration=10:size=1280x720:rate=30 test.mp4
```

Open index.html in the browser.

Run the `whep` executable to start the http server. You should be prompted with a message saying the application is running on port 8080.

Once the http server is up, the the "Subscribe" button the begin the signaling.

When the peers sucessfully connect we can now send video the open UDP port running on port 6000.

```sh
$ ffmpeg -re -i test.mp4 -c:v libvpx -c:a aac -f rtp udp://127.0.0.1:6000
```

Now you should see video on the video player in the browser

## Using the WHIP example

Open index.html in the browser.

Run the `whip` executable to start the http server. You should be prompted with a message saying the application is running on port 8080.

Once the http server is up, the the "Publish" button the begin the signaling.


After the connnection is made the libdatachannel peer will be writing the RTP packets to a UDP port running on port 5000. We now need to intercept those packets and write it to an mp4 file.

```sh
$ ffmpeg -protocol_whitelist "file,udp,rtp" -i stream.sdp -c:v copy output.webm
```
