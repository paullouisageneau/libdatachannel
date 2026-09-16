# SFrame end-to-end encrypted media

A sender and a receiver, both libdatachannel, exchanging SFrame-protected audio and video
(RFC 9605) over the RTP transport of draft-ietf-avtcore-rtp-sframe.

SFrame encrypts the media itself, so it stays protected across a relay or SFU that terminates
DTLS-SRTP. The two peers here talk directly, but nothing in the media path depends on that.

## What to look at

Both m-lines are `sendrecv`, so media flows in both directions and each direction is encrypted under
its own key. That is the point of the pair: RFC 9605 Section 4.4.1 forbids one base key being used
for encryption by more than one sender, so each peer generates its own sending key and never
encrypts with the one it received.

Each m-line also uses a different cipher suite — a long authentication tag on video, a short one on
audio — which RFC 9605 Section 4.5 gives as its own example. A provider fixes its suite at
construction, so that is why these are installed per track with `Track::useSFrame()` rather than once
for the session with `PeerConnection::useSFrame()`.

- `sender.cpp` offers both m-lines, and offers **two video codecs** so the answer has a real choice
  to make. It builds its media chain only once the answer has been applied, because one thing is
  settled by the answer and not by this end: which codec was accepted. The alternative — building it
  upfront — is described in the comment there, along with the condition that makes it safe. It also
  offers RTX and chains an `RtcpNackResponder`, so a lost frame is retransmitted per RFC 4588.
- `receiver.cpp` answers. In `onTrack` it does three things the library will not do for it: it
  **narrows the codec list to one**, it **adds its own SSRC to the answer**, and it installs the
  SFrame handlers with `Track::useSFrame()`, which is what keeps `a=sframe` in the answer. It prefers
  VP8 over H.264 — the reverse of the offer's order — so running the pair shows the sender genuinely
  reading the choice out of the answer rather than assuming its own first preference.
- `sframecommon.hpp` generates fresh random sending keys per run and carries the paste-based
  signalling. A receive provider returns `nullopt` for a key generation it does not recognise, which
  is what makes the decoder refuse forged frames — and until the peer's key has been pasted in, its
  frames are refused rather than guessed at.

**There is no key in this source.** Each run mints random base keys and key generations, prints them,
and you copy them to the other peer with command `3`. An example with a hard-coded key gets copied
into real products, and a key in source control is a key everyone has. A real deployment takes both
directions' keys from a group key agreement such as MLS, or from a key server.

## Two things the library leaves to the application

**Codec narrowing.** `reciprocate()` copies every `a=rtpmap` the offer carried into the answer, and
the answer's m-line is then taken verbatim from the track description. Nothing in the library removes
a codec the answerer will not use, so an answerer that leaves the list alone accepts every codec
offered and gives the offerer no way to tell which one to send. `receiver.cpp` shows the narrowing,
including keeping an RTX mapping whose `apt` names the codec being kept.

**KID lifecycle.** The library derives keys, encrypts, and looks keys up by generation. It does not
invent, reuse or retire a KID. `rollKey()` accepts a KID it has sent under before so long as the
material is new — which the RFC 9605 Section 5.2 MLS layout needs, since that KID carries only the
low bits of the MLS epoch and its values necessarily recur. Handing it back the key already in force
is ignored rather than treated as a roll: adopting a key restarts the counter, so a stray re-supply
would otherwise replay every nonce already emitted under that key and KID. What no API can check,
and what the key source must guarantee, is that a key is never re-supplied with a counter at or below
one already spent. How to resume depends on the mode. With per-SSRC derivation off there is one
counter for the session, and `currentKey()` reports where sending has reached: persist it and hand it
to the constructor, and the provider resumes rather than restarts. With derivation on — which is what
this example uses — the counter belongs to each track's derived key, so `currentKey()` reports
nothing and the way to resume is to roll to a key generation never used before: the KID feeds the
HKDF for both the key and the salt, so a fresh generation is a fresh nonce space.

On the receive side, `addKey()` and `removeKey()` are the application's to call. A provider holds any
number of generations at once, so a multi-party call can register every participant's key up front,
and one provider serves every track — including m-lines that arrive later as people join. Nothing is
dropped on its own: a generation stays until `removeKey()` drops it, so call it when a participant
leaves. The library cannot make that call for you — it sees a KID and an SSRC, and behind an
SFU the SSRC does not identify a sender, so a change of generation means the speaker changed rather
than that anyone rekeyed. `keyAuthenticated()` reports which generation is in use if you want to
drive a policy from traffic.

## When to build the media chain

This is the decision `sender.cpp` is built around, and it is worth copying deliberately.

- **Deferred** (what the example does). Nothing is installed until the answer has been applied, so
  the packetizer is built for the payload type the answer actually chose. Nothing is lost by waiting:
  `Track::isOpen()` is false until the DTLS-SRTP transport exists, so no frame could have gone out
  earlier anyway.
- **Upfront.** Correct where the codec cannot change, which takes one codec per m-line. Nothing is
  needed for `a=sframe`: an answer that declines it stops the m-line rather than downgrading it, so
  an upfront chain cannot end up sending in the clear. Given that, `SFramePerFrameRtpPacketizer` is
  safe to install early precisely because it is codec-agnostic: it encrypts the whole frame and
  splits the result
  into MTU-sized chunks, so no codec-specific framing is involved.

## The receive-side handler chain

`onTrack` is where the application adds its handlers, and the two branches differ:

- **SFrame negotiated.** `useSFrame()` puts SFrame at the head of the chain, so `chainMediaHandler()`
  puts anything added afterwards *behind* it. That is the order the receive path needs: incoming
  handlers run from the tail of the chain toward the head, so an `RtcpReceivingSession` unwraps an
  RTX retransmission — restoring the original SSRC and sequence number — before SFrame reads the
  descriptor byte. The same handler carries NACK, PLI and RTCP reports.
- **SFrame not negotiated.** This is an ordinary track and wants the negotiated codec's own
  depacketizer — the stage SFrame stands in for when it is on. The SFrame depacketizers always
  decrypt, so one of them cannot stand in for a codec depacketizer here.

Handlers installed *before* `useSFrame()` are kept: SFrame goes in front and the rest stays behind
it. The one exception is a codec depacketizer, which occupies the same stage as SFrame and is
replaced, with an info-level log line naming the m-line. A packetizer is not affected — it is the
outgoing stage, and a `sendrecv` track needs one alongside.

**SFrame does not distribute keys.** That is why the key exchange here is manual: it is the one part
a real deployment must replace with its own key management.

## How to use

Start both, then copy the SDP and candidates between the two terminals — the receiver answers the
sender's offer.

```
$ ./sframe-receiver
$ ./sframe-sender
```

1. Each side prints its own sending keys at startup, one line per m-line. Paste each into the other
   with command `3`.
2. The sender prints its local description. Paste it into the receiver with command `1`, ending with
   a blank line.
3. The receiver prints its answer. Paste it into the sender with command `1`.
4. Paste each side's candidates into the other with command `2`.

The receiver prints which codec it chose (`[Chose VP8, payload type 98 on mid=video]`) and the sender
prints what it read back out of the answer (`[video: payload type 98, SFrame on]`). Both sides then
print `[Sent video frame N]` and `[Decrypted video frame N: ...]`, each decrypting the other's media
under the key it was given.

To see that the media really is encrypted, comment out the `videoMedia.addSFrame()` and
`audioMedia.addSFrame()` calls in `sender.cpp`. The offer then carries no `a=sframe` at all, so the
receiver treats the tracks as ordinary ones, installs the codec's own handlers, and the frames cross
the wire in the clear. Note this works because the attribute was never offered -- an m-line that
offers it and is refused is stopped rather than downgraded.
