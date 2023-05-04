const iceConnectionLog = document.getElementById('ice-connection-state'),
    iceGatheringLog = document.getElementById('ice-gathering-state'),
    signalingLog = document.getElementById('signaling-state'),
    dataChannelLog = document.getElementById('data-channel');

const clientId = randomId(10);
const websocket = new WebSocket('ws://127.0.0.1:8000/' + clientId);

websocket.onopen = () => {
    document.getElementById('start').disabled = false;
}

websocket.onmessage = async (evt) => {
    if (typeof evt.data !== 'string') {
        return;
    }
    const message = JSON.parse(evt.data);
    if (message.type == "offer") {
        document.getElementById('offer-sdp').textContent = message.sdp;
        await handleOffer(message)
    }
}

let pc = null;
let dc = null;

function createPeerConnection() {
    const config = {
        bundlePolicy: "max-bundle",
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
        const video = document.getElementById('video');
        // always overrite the last stream - you may want to do something more clever in practice
        video.srcObject = evt.streams[0]; // The stream groups audio and video tracks
        video.play();
    };

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

async function sendAnswer(pc) {
    await pc.setLocalDescription(await pc.createAnswer());
    await waitGatheringComplete();

    const answer = pc.localDescription;
    document.getElementById('answer-sdp').textContent = answer.sdp;

    websocket.send(JSON.stringify({
        id: "server",
        type: answer.type,
        sdp: answer.sdp,
    }));
}

async function handleOffer(offer) {
    pc = createPeerConnection();
    await pc.setRemoteDescription(offer);
    await sendAnswer(pc);
}

function sendRequest() {
    websocket.send(JSON.stringify({
        id: "server",
        type: "request",
    }));
}

function start() {
    document.getElementById('start').style.display = 'none';
    document.getElementById('stop').style.display = 'inline-block';
    document.getElementById('media').style.display = 'block';
    sendRequest();
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

