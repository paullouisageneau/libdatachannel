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
            const answererUrl = 'http://localhost:8080/answerer.html?connection=' + btoa(JSON.stringify(connectionInfo));
            const createLink = document.createElement('a');
            createLink.setAttribute('href', answererUrl);
            createLink.setAttribute('target', 'new');
            createLink.append('Open me ;)');
            document.body.append(createLink);
            const pollForConnection = setInterval(() => {
                fetch(
                    'http://localhost:8000/state/json'
                ).then(response => {
                    if (response.status !== 200) {
                        throw new Error('bad status ' + response.status);
                    }
                    return response.json();
                }).then(data => {
                    if (data.description) {
                        console.log('yes');
                        clearInterval(pollForConnection);
                        createRemoteConnection(data.description);
                    }
                    else {
                        console.log('no');
                    }
                })
            }, 5000)
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

    function createRemoteConnection(desc) {
        const remoteDesc = {
            type: "answer",
            sdp: desc
        }
        localConnection.setRemoteDescription(remoteDesc).then((e) => {
            console.log(e)
        });
    }

    const sendButton = document.getElementById('sendDataBtn')
    sendButton.addEventListener('click', sendMessage, false)
    function sendMessage() {
        const messageText = document.getElementById('sendData').value;
        sendChannel.send(messageText);   
    }
}


