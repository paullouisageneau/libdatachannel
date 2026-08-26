# TURN/DTLS ABBA deadlock reproduction

This standalone example reproduces the TURN/DTLS deadlock symptom 
when using OpenSSL backend.


## What it does

Both peers are created in a single process and forced onto the TURN relay path
(TransportPolicy::Relay) through one fixed TURN instance. The offerer (pc1) ends
up as the DTLS server (passive) role -- the side that is vulnerable to the
deadlock. The answerer (pc2) is the DTLS client (active) role and sends its
ClientHello early.

Because both peers share the process, the relay RTT is tiny and the remote
ClientHello routinely arrives in the same millisecond as the local
DtlsTransport::start() call. That is exactly the pre-queue race described in the
report:

- juice poll thread holds the libjuice registry->mutex (callback context) and
  waits on mSslMutex inside start() / handleTimeout().
- ThreadPool doRecv thread holds mSslMutex, sends the ServerHello flight and
  blocks in juice_send() waiting on conn_lock (== the same registry->mutex) --
  but only on the relay send path.

=> ABBA deadlock. ICE selects the relay pair, but DTLS never completes and the
connection dies at the ~30s consent-expiry timeout.

## Build

Configure the project with examples enabled (the default) and build the
datachannel-turn-dtls-deadlock target:

    cmake -B build -DNO_EXAMPLES=OFF
    cmake --build build --target datachannel-turn-dtls-deadlock

## Run

Provide your TURN credential via the RTC_TURN environment variable, then run
with an optional iteration count (default 50):

    # Linux / macOS
    export RTC_TURN="turn:<username>:<credential>@turn.server.ip.address:3478"
    ./turn-dtls-deadlock 100

    :: Windows
    set RTC_TURN=turn:<username>:<credential>@turn.server.ip.address:3478
    turn-dtls-deadlock.exe 100

## Interpreting the output

- Unpatched library (calling handleTimeout() at the end of
  DtlsTransport::start()): the first stalled attempt prints "DTLS handshake did
  NOT complete ..." with "ICE relay pair selected but DTLS never finishes",
  immediately followed by a summary line, and the process terminates right away
  with status 1 (via std::_Exit).

  The run intentionally stops at the FIRST stall: a wedged attempt can never be
  torn down -- close() would block on the very mutexes the deadlocked threads
  hold forever -- so its objects are intentionally leaked instead. Moreover,
  the wedged "juice poll" thread keeps holding libjuice's global conn registry
  mutex, which is shared by every agent in the process, so creating the next
  attempt's PeerConnection would itself hang forever on that mutex (confirmed
  by a crash dump of a hung run). To collect repeat measurements, run the
  executable several times (one process per measurement), e.g. in a shell
  loop, instead of passing a large iteration count.

- Patched library (start() defers the initial timeout handling to the
  ThreadPool): every attempt completes the handshake in a few milliseconds, the
  summary reports 0/N stalls and the process exits with status 0.