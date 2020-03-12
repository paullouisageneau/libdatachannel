(function (){
    const urlParams = new URLSearchParams(window.location.search);
    const connectionParam = urlParams.get('connection')
    console.log(atob(connectionParam));
    const decoded = atob(connectionParam);
    const bits = decoded.split('xxxxx');
    const connection = {description: bits[0], candidate: bits[1]}
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
    let connectionDescription = '';

    remoteConnection.setRemoteDescription(remoteDesc).then((e) => {
        console.log(e)
    });
    remoteConnection.addIceCandidate(remoteCandidate).then(
        () => {console.log('yes')}
    )

    remoteConnection.createAnswer().then((desc) => {
        remoteConnection.setLocalDescription(desc);
        console.dir(desc);
        connectionDescription = desc.sdp;
    })


    remoteConnection.onicecandidate = e => {
        if (e.candidate) {
            const candidate = e.candidate.candidate;
            const body = connectionDescription;
            fetch(
                'http://localhost:8000/state/json',
                {
                    method: 'post',
                    headers: {'Content-Type': 'text/plain'},
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
        const sendButton = document.getElementById('sendDataBtn')
        sendButton.addEventListener('click', sendMessage, false)
        function sendMessage() {
            const messageText = document.getElementById('sendData').value;
            receiveChannel.send(messageText);   
        }
        receiveChannel.onopen = () => {
            console.log('channel open')
            receiveChannel.send('testing testing 123'); 
        }
        receiveChannel.onmessage = (msg) => {
            document.body.append(msg.data);
        }
    }
}


