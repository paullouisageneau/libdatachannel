(function (){
    console.log('Hello');
    const connectRemote = document.getElementById('connectRemote')
    connectRemote.addEventListener('click', createRemoteConnection, false)
})();

function createRemoteConnection() {
    const remoteDescText = document.getElementById('remoteDesc').value
    const remoteDescJSON = JSON.parse(remoteDescText);
    const remoteDesc = {
        type: "offer",
        sdp: remoteDescJSON.description 
    }
    const remoteCandidate = {
        candidate: remoteDescJSON.candidate,
        sdpMid: "0", // Media stream ID for audio
        sdpMLineIndex: 0 // Something to do with media
    }
    const remoteConnection = new RTCPeerConnection();
    const connectionInfo = {};

    remoteConnection.setRemoteDescription(remoteDesc).then((e) => {
        console.log(e)
    });
    remoteConnection.addIceCandidate(remoteCandidate).then(
        () => {console.log('yes')}
    )

    remoteConnection.createAnswer().then((desc) => {
        remoteConnection.setLocalDescription(desc);
        console.dir(desc);
        connectionInfo.description = desc.sdp;
    })


    remoteConnection.onicecandidate = e => {
        if (e.candidate) {
            const candidate = e.candidate.candidate;
            connectionInfo.candidate = candidate;
            document.body.append(JSON.stringify(connectionInfo));
        }
    }
    remoteConnection.onicecandidateerror = err => {
        console.log(err)
    }
    remoteConnection.ondatachannel = (e) => {
        console.log('onDataChannel')
        const receiveChannel = e.channel;
        console.dir(receiveChannel);
        receiveChannel.onmessage = (msg) => {
            document.body.append(msg.data);
        }
    }
}


