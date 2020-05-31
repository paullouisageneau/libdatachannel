function el(type, style, content) {
  const element = document.createElement(type);
  element.setAttribute('class', style);
  element.append(content);
  return element;
}

const UI = {
  createMessageElement: function(style, message) {
    const conversation =  document.getElementById('messages');
    const messageElement = el('p', style + ' message', message);
    conversation.append(messageElement);

    const d = new Date();
    const time = el('span', 'time', [
      d.getHours().toString().padStart(2, '0'),
      d.getMinutes().toString().padStart(2, '0'),
      d.getSeconds().toString().padStart(2, '0')
    ].join(':'));
    messageElement.append(time);
  },
  messageTextBoxValue: function() {
    return document.getElementById('message-text-box').value;
  },
  sendMessageButton: function() {
    return document.getElementById('send-message-button');
  },
  setConnectionState: function(state) {
    const stateEl = document.getElementById('connection-state');
    stateEl.innerHTML = state;
  }
};
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
    UI.setConnectionState('received channel: ' + JSON.stringify(e));
    const channel = e.channel;
    console.log(channel);
    const sendButton = UI.sendMessageButton();
    sendButton.addEventListener('click', sendMessage.bind(null, channel), false);
    channel.onopen = () => {
      console.log('open');
      UI.setConnectionState('channel open');
    };
    channel.onmessage = (msg) => {
      UI.createMessageElement('from-them', msg.data);
    };
  };
}

function sendMessage(channel) {
  const message = UI.messageTextBoxValue();
  UI.createMessageElement('from-me', message);
  channel.send(message);
}

function getQueryParameter(name) {
  const urlParams = new URLSearchParams(window.location.search);
  return urlParams.get(name);
}
