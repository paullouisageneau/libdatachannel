(function (){
  const connectionParam = getQueryParameter('connection');
  const parsedConnection = atob(connectionParam).split(',');
  const connectionMetadata = {
    description: parsedConnection[0],
    candidate: parsedConnection[1]
  };
  createRemoteConnection(connectionMetadata);
})();

function createRemoteConnection(connectionMetadata) {
  console.log(connectionMetadata);
  const connection = new RTCPeerConnection();

  const remoteDesc = {
    type: 'offer',
    sdp: connectionMetadata.description
  };
  connection.setRemoteDescription(remoteDesc)
    .then(() => UI.setConnectionState('set remote description'));
  let connectionDescription = '';

  const remoteCandidate = {
    candidate: connectionMetadata.candidate,
    sdpMid: '0', // Media stream ID for audio
    sdpMLineIndex: 0 // Something to do with media
  };
  connection.addIceCandidate(remoteCandidate).then(
    () => UI.setConnectionState('candidate added')
  );

  connection.createAnswer().then((desc) => {
    connection.setLocalDescription(desc);
    connectionDescription = desc.sdp;
  });

  connection.onicecandidate = e => {
    if (e.candidate) {
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
      });
    }
  };
  connection.ondatachannel = (e) => {
    UI.setConnectionState('received channel');
    const channel = e.channel;
    console.dir(channel);
    const sendButton = document.getElementById('sendDataBtn');
    sendButton.addEventListener('click', sendMessage.bind(null, channel), false);
    channel.onopen = () => {
      console.log('channel open');
      UI.setConnectionState('channel open');
    };
    channel.onmessage = (msg) => {
      UI.createMessageElement('from-them', msg.data);
    };
  };
}

const UI = {
  createMessageElement: function(style, message) {
    const conversation =  document.getElementById('messages');
    const messageElement = document.createElement('p');
    messageElement.setAttribute('class', style);
    messageElement.append(message);
    conversation.append(messageElement);
  },
  messageInputValue: function() {
    return document.getElementById('sendData').value;
  },
  setConnectionState: function(state) {
    const stateEl = document.getElementById('connection-state');
    stateEl.innerHTML = state;
  }
};

function sendMessage(channel) {
  const message = UI.messageInputValue();
  UI.createMessageElement('from-me', message);
  channel.send(message);
}

function getQueryParameter(name) {
  const urlParams = new URLSearchParams(window.location.search);
  return urlParams.get(name);
}
