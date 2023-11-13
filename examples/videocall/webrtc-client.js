const iceConnectionLog = document.getElementById('ice-connection-state'),
    iceGatheringLog = document.getElementById('ice-gathering-state'),
    signalingLog = document.getElementById('signaling-state'),
    dataChannelLog = document.getElementById('data-channel');

const clientId = randomId(10);
document.getElementById("my-id").innerHTML = clientId;

const websocket = new WebSocket('ws://10.196.28.10:8888/' + 'join/' + clientId);

const remotePeer = null;

let localstream = null;

websocket.onopen = () => {
    document.getElementById('start').disabled = false;
}

let pc = null;
let dc = null;

async function hangup() {
    if (pc) {
      pc.close();;
      pc = null;
    }

    // localStream.getTracks().forEach(track => track.stop());
    // localStream = null;
    // startButton.disabled = false;
    // hangupButton.disabled = true;

    // TODO stop track
    // TODO show end session
}

function handleSignalingMsg(message) {

    const peerId = message.id;

    switch (message.type) {
        case 'offer':
          handleOffer({type : message.type, sdp : message.sdp}, peerId);
          break;
        case 'answer':
          handleAnswer({type : message.type, sdp : message.sdp}, peerId);
          break;
        case 'candidate':
          handleCandidate(message);
          break;
        case 'ready':
          // A second tab joined. This tab will initiate a call unless in a call already.
        //   if (pc) {
        //     console.log('already in call, ignoring');
        //     return;
        //   }
        //   makeCall();
        //   break;
        case 'bye':
        case 'useroffline':
        case 'userbusy':
          if (pc) {
            hangup();
          }
          break;
        default:
          console.log('unhandled', e);
          break;
    }
}

websocket.onmessage = async (evt) => {
    if (typeof evt.data !== 'string') {
        return;
    }
    const message = JSON.parse(evt.data);


    handleSignalingMsg(message);
}

async function createPeerConnection() {
    const config = {
        // bundlePolicy: "max-bundle",
    };

    if (document.getElementById('use-stun').checked) {
        config.iceServers = [{urls: ['stun:stun.l.google.com:19302']}];
    }

    let pc = new RTCPeerConnection(config);

    // Register some listeners to help debugging
    pc.addEventListener('iceconnectionstatechange', () =>
        iceConnectionLog.textContent += ' -> ' + pc.iceConnectionState);
    iceConnectionLog.textContent = pc.iceConnectionState;

    pc.addEventListener('icegatheringstatechange', () =>
        iceGatheringLog.textContent += ' -> ' + pc.iceGatheringState);
    iceGatheringLog.textContent = pc.iceGatheringState;

    pc.addEventListener('signalingstatechange', () =>
        signalingLog.textContent += ' -> ' + pc.signalingState);
    signalingLog.textContent = pc.signalingState;

    // Receive audio/video track
    pc.ontrack = (evt) => {
        document.getElementById('media').style.display = 'block';
        const peervideo = document.getElementById('video-peer');
        // always overrite the last stream - you may want to do something more clever in practice

        if (!peervideo.srcObject) {
            peervideo.srcObject = evt.streams[0]; // The stream groups audio and video tracks
            peervideo.play();
        }
    };

    localstream.getTracks().forEach(track => pc.addTrack(track, localstream));

    // Receive data channel
    pc.ondatachannel = (evt) => {
        dc = evt.channel;

        dc.onopen = () => {
            dataChannelLog.textContent += '- open\n';
            dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        };

        let dcTimeout = null;
        dc.onmessage = (evt) => {
            if (typeof evt.data !== 'string') {
                return;
            }

            dataChannelLog.textContent += '< ' + evt.data + '\n';
            dataChannelLog.scrollTop = dataChannelLog.scrollHeight;

            dcTimeout = setTimeout(() => {
                if (!dc) {
                    return;
                }
                const message = `Pong ${currentTimestamp()}`;
                dataChannelLog.textContent += '> ' + message + '\n';
                dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
                dc.send(message);
            }, 1000);
        }

        dc.onclose = () => {
            clearTimeout(dcTimeout);
            dcTimeout = null;
            dataChannelLog.textContent += '- close\n';
            dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        };
    }

    return pc;
}

async function waitGatheringComplete() {
    return new Promise((resolve) => {
        if (pc.iceGatheringState === 'complete') {
            resolve();
        } else {
            pc.addEventListener('icegatheringstatechange', () => {
                if (pc.iceGatheringState === 'complete') {
                    resolve();
                }
            });
        }
    });
}

async function sendAnswer(pc, peerId) {
    await pc.setLocalDescription(await pc.createAnswer());
    await waitGatheringComplete();

    const answer = pc.localDescription;
    document.getElementById('answer-sdp').textContent = answer.sdp;

    websocket.send(JSON.stringify({
        id: peerId,
        type: answer.type,
        sdp: answer.sdp,
    }));
}

async function handleOffer(offer, peerId) {
    if (pc) {
        console.error('existing peerconnection');
        return;
    }

    pc = await createPeerConnection();
    document.getElementById('offer-sdp').textContent = offer.sdp;

    await pc.setRemoteDescription(offer);
    await sendAnswer(pc, peerId);
}

async function handleAnswer(answer, peerId) {
    if (!pc) {
        console.log("No existing peerconn!");
        return;
    }

    await pc.setRemoteDescription(answer);
    console.log("set remote desc sdp done");
}

async function sendRequest() {
    if (!peerID) {
        console.log("Failed to send videocall request, null peerID");
    }

    pc = await createPeerConnection();

    myOffer = await pc.createOffer();

    // we should get generate our local sdp and send it to the remote end
    websocket.send(JSON.stringify({
        id : peerID,
        type : "offer",
        sdp : myOffer.sdp
    }));

    pc.setLocalDescription(myOffer);
}

async function getMedia(constraints) {
    try {
      localstream = await navigator.mediaDevices.getUserMedia(constraints);
      /* use the stream */
      console.log("Got user media");

      document.getElementById('media').style.display = 'block';
      const myVideo = document.getElementById('video-me');

      myVideo.srcObject = localstream;
      myVideo.play()

    } catch (err) {
      /* handle the error */
      console.log(err);
    }
}

async function start() {
    peerID = document.getElementById('peerID').value;

    if (!peerID) {
        alert("Please input peerID before calling");
        return;
    }

    document.getElementById('start').style.display = 'none';
    document.getElementById('stop').style.display = 'inline-block';

    await sendRequest();

    console.log("3");
}

function stop() {
    document.getElementById('stop').style.display = 'none';
    document.getElementById('media').style.display = 'none';
    document.getElementById('start').style.display = 'inline-block';

    // close data channel
    if (dc) {
        dc.close();
        dc = null;
    }

    // close transceivers
    if (pc.getTransceivers) {
        pc.getTransceivers().forEach((transceiver) => {
            if (transceiver.stop) {
                transceiver.stop();
            }
        });
    }

    // close local audio/video
    pc.getSenders().forEach((sender) => {
        const track = sender.track;
        if (track !== null) {
            sender.track.stop();
        }
    });

    // close peer connection
    pc.close();
    pc = null;
}

// Helper function to generate a random ID
function randomId(length) {
  const characters = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
  const pickRandom = () => characters.charAt(Math.floor(Math.random() * characters.length));
  return [...Array(length) ].map(pickRandom).join('');
}

// Helper function to generate a timestamp
let startTime = null;
function currentTimestamp() {
    if (startTime === null) {
        startTime = Date.now();
        return 0;
    } else {
        return Date.now() - startTime;
    }
}

const constraints = {
    video : true,
    audio : true
}

getMedia(constraints);

