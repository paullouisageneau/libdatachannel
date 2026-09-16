# SFrame end-to-end encrypted media

A sender and a receiver, both libdatachannel, exchanging a video track protected with SFrame
(RFC 9605) over the RTP transport of draft-ietf-avtcore-rtp-sframe.

SFrame encrypts the media itself, so it stays protected across a relay or SFU that terminates
DTLS-SRTP. The two peers here talk directly, but nothing in the media path depends on that.

## What to look at

- `sender.cpp` builds an `SFrameRtpPacketizer` and installs it as the track's media handler.
  It replaces the codec packetizer entirely: the frame is encrypted whole and then split into
  MTU-sized chunks, each carrying the 1-byte SFrame descriptor, so one packetizer serves both
  audio and video.
- `receiver.cpp` checks `hasSFrame()` on the track description from its `onTrack` callback --
  the offer's attribute reaches it there -- and installs an `SFrameVideoRtpDepacketizer` with a
  key provider only if the peer asked for SFrame. The answer keeps `a=sframe` only because that
  handler was installed: with nothing in the chain applying SFrame the attribute is stripped, so
  an answer never claims protection the application has not provided.
- `common.hpp` holds the key material and the `SFrameKeyProvider`. Note it returns `nullopt`
  for a key generation it does not recognise, which is what makes the decoder refuse forged
  frames, and that each direction has its own base key and key generation -- RFC 9605 Section
  4.4.1 forbids one base key being used for encryption by more than one sender.

**SFrame does not distribute keys.** Both peers here are compiled with the same hard-coded key
so the example needs no extra signalling. A real deployment gets it from a group key agreement
such as MLS, or from a key server.

## How to use

Start both, then copy the SDP and candidates between the two terminals — the receiver answers
the sender's offer.

```
$ ./sframe-receiver
$ ./sframe-sender
```

1. The sender prints its local description. Paste it into the receiver with command `1`,
   ending with a blank line.
2. The receiver prints its answer. Paste it into the sender with command `1`.
3. Paste each side's candidates into the other with command `2`.

The sender then prints `[Sent frame N]` and the receiver prints `[Decrypted frame N: ...]`.

To see that the media really is encrypted, comment out the `media.addSFrame()` call in
`sender.cpp`. The offer then carries no `a=sframe`, the receiver sees that and installs no
SFrame handler, and the frames cross the wire in the clear.
