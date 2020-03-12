(function (){
    const urlParams = new URLSearchParams(window.location.search);
    const connectionParam = urlParams.get('connection')
    const connection = JSON.parse(atob(connectionParam));
    console.dir(connection);
    createRemoteConnection(connection);
})();

function createRemoteConnection(remoteDescJSON) {
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
            const body = JSON.stringify(connectionInfo)
            fetch(
                'http://localhost:8000/state/json',
                {
                    method: 'post',
                    headers: {'Content-Type': 'application/json'},
                    body
                }
            ).then(response => {
                if (response.status !== 200) {
                    throw new Error('bad status ' + response.status);
                }
            })
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


