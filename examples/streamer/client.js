/** @type {RTCPeerConnection} */
let rtc;
const iceConnectionLog = document.getElementById('ice-connection-state'),
    iceGatheringLog = document.getElementById('ice-gathering-state'),
    signalingLog = document.getElementById('signaling-state'),
    dataChannelLog = document.getElementById('data-channel');

function randomString(len) {
    const charSet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    let randomString = '';
    for (let i = 0; i < len; i++) {
        const randomPoz = Math.floor(Math.random() * charSet.length);
        randomString += charSet.substring(randomPoz, randomPoz + 1);
    }
    return randomString;
}

const receiveID = randomString(10);
const websocket = new WebSocket('ws://127.0.0.1:8000/' + receiveID);
websocket.onopen = function () {
    document.getElementById('start').disabled = false;
}

// data channel
let dc = null, dcTimeout = null;

function createPeerConnection() {
    const config = {
        sdpSemantics: 'unified-plan',
        bundlePolicy: "max-bundle",
    };

    if (document.getElementById('use-stun').checked) {
        config.iceServers = [{urls: ['stun:stun.l.google.com:19302']}];
    }

    let pc = new RTCPeerConnection(config);

    // register some listeners to help debugging
    pc.addEventListener('icegatheringstatechange', function () {
        iceGatheringLog.textContent += ' -> ' + pc.iceGatheringState;
    }, false);
    iceGatheringLog.textContent = pc.iceGatheringState;

    pc.addEventListener('iceconnectionstatechange', function () {
        iceConnectionLog.textContent += ' -> ' + pc.iceConnectionState;
    }, false);
    iceConnectionLog.textContent = pc.iceConnectionState;

    pc.addEventListener('signalingstatechange', function () {
        signalingLog.textContent += ' -> ' + pc.signalingState;
    }, false);
    signalingLog.textContent = pc.signalingState;

    // connect audio / video
    pc.addEventListener('track', function (evt) {
        document.getElementById('media').style.display = 'block';
        const videoTag = document.getElementById('video');
        videoTag.srcObject = evt.streams[0];
        videoTag.play();
    });

    let time_start = null;

    function current_stamp() {
        if (time_start === null) {
            time_start = new Date().getTime();
            return 0;
        } else {
            return new Date().getTime() - time_start;
        }
    }

    pc.ondatachannel = function (event) {
        dc = event.channel;
        dc.onopen = function () {
            dataChannelLog.textContent += '- open\n';
            dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        };
        dc.onmessage = function (evt) {

            dataChannelLog.textContent += '< ' + evt.data + '\n';
            dataChannelLog.scrollTop = dataChannelLog.scrollHeight;

            dcTimeout = setTimeout(function () {
                if (dc == null && dcTimeout != null) {
                    dcTimeout = null;
                    return
                }
                const message = 'Pong ' + current_stamp();
                dataChannelLog.textContent += '> ' + message + '\n';
                dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
                dc.send(message);
            }, 1000);
        }
        dc.onclose = function () {
            clearTimeout(dcTimeout);
            dcTimeout = null;
            dataChannelLog.textContent += '- close\n';
            dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        };
    }

    return pc;
}

function sendAnswer(pc) {
    return pc.createAnswer()
        .then((answer) => rtc.setLocalDescription(answer))
        .then(function () {
            // wait for ICE gathering to complete
            return new Promise(function (resolve) {
                if (pc.iceGatheringState === 'complete') {
                    resolve();
                } else {
                    function checkState() {
                        if (pc.iceGatheringState === 'complete') {
                            pc.removeEventListener('icegatheringstatechange', checkState);
                            resolve();
                        }
                    }

                    pc.addEventListener('icegatheringstatechange', checkState);
                }
            });
        }).then(function () {
            const answer = pc.localDescription;

            document.getElementById('answer-sdp').textContent = answer.sdp;

            return websocket.send(JSON.stringify(
                {
                    id: "server",
                    type: answer.type,
                    sdp: answer.sdp,
                }));
        }).catch(function (e) {
            alert(e);
        });
}

function handleOffer(offer) {
    rtc = createPeerConnection();
    return rtc.setRemoteDescription(offer)
        .then(() => sendAnswer(rtc));
}

function sendStreamRequest() {
    websocket.send(JSON.stringify(
        {
            id: "server",
            type: "streamRequest",
            receiver: receiveID,
        }));
}

async function start() {
    document.getElementById('start').style.display = 'none';
    document.getElementById('stop').style.display = 'inline-block';
    document.getElementById('media').style.display = 'block';
    sendStreamRequest();
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
    if (rtc.getTransceivers) {
        rtc.getTransceivers().forEach(function (transceiver) {
            if (transceiver.stop) {
                transceiver.stop();
            }
        });
    }

    // close local audio / video
    rtc.getSenders().forEach(function (sender) {
        const track = sender.track;
        if (track !== null) {
            sender.track.stop();
        }
    });

    // close peer connection
    setTimeout(function () {
        rtc.close();
        rtc = null;
    }, 500);
}


websocket.onmessage = async function (evt) {
    const received_msg = evt.data;
    const object = JSON.parse(received_msg);
    if (object.type == "offer") {
        document.getElementById('offer-sdp').textContent = object.sdp;
        await handleOffer(object)
    }
}
