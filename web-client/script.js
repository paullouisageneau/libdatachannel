(function (){
    console.log('Hello');
    const connectButton = document.getElementById('createConnectionBtn')
    connectButton.addEventListener('click', createConnection, false)
})();

function createConnection() {
    const localConnection = new RTCPeerConnection();
    const sendChannel = localConnection.createDataChannel('channel');
    console.dir(sendChannel);
    sendChannel.onopen = e => {
        console.log('open')
        console.log(e)
    }
    sendChannel.onclose = e => {
        console.log('close')
        console.log(e)
    }
    const connectionInfo = {};
    localConnection.onicecandidate = e => {
        if (e.candidate) {
            const candidate = e.candidate.candidate;
            connectionInfo.candidate = candidate;
            document.body.append(JSON.stringify(connectionInfo));
        }
    }
    localConnection.onicecandidateerror = err => {
        console.log(err)
    }
    
    localConnection.createOffer().then((desc) => {
            connectionInfo.description = desc.sdp;
            localConnection.setLocalDescription(desc);
        }
    )
    const connectRemote = document.getElementById('connectRemote')

    function createRemoteConnection() {
        const remoteDescText = document.getElementById('remoteDesc').value
        const remoteDescJSON = JSON.parse(remoteDescText);
        const remoteDesc = {
            type: "answer",
            sdp: remoteDescJSON.description 
        }
        const remoteCandidate = {
            candidate: remoteDescJSON.candidate,
            sdpMid: "0", // Media stream ID for audio
            sdpMLineIndex: 0 // Something to do with media
        }
        console.dir(remoteDescJSON)
        localConnection.setRemoteDescription(remoteDesc).then((e) => {
            console.log(e)
        });
    }

    connectRemote.addEventListener('click', createRemoteConnection, false)

    const sendButton = document.getElementById('sendDataBtn')
    sendButton.addEventListener('click', sendMessage, false)
    function sendMessage() {
        const messageText = document.getElementById('sendData').value;
        sendChannel.send(messageText);   
    }
}


