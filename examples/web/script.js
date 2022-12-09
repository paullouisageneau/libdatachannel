/*
 * libdatachannel example web client
 * Copyright (C) 2020 Lara Mackey
 * Copyright (C) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

window.addEventListener('load', () => {

const config = {
  iceServers : [ {
    urls : 'stun:stun.l.google.com:19302', // change to your STUN server
  } ],
};

const localId = randomId(4);

const url = `ws://localhost:8000/${localId}`;

const peerConnectionMap = {};
const dataChannelMap = {};

const offerId = document.getElementById('offerId');
const offerBtn = document.getElementById('offerBtn');
const sendMsg = document.getElementById('sendMsg');
const sendBtn = document.getElementById('sendBtn');
const _localId = document.getElementById('localId');
_localId.textContent = localId;

console.log('Connecting to signaling...');
openSignaling(url)
    .then((ws) => {
      console.log('WebSocket connected, signaling ready');
      offerId.disabled = false;
      offerBtn.disabled = false;
      offerBtn.onclick = () => offerPeerConnection(ws, offerId.value);
    })
    .catch((err) => console.error(err));

function openSignaling(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.onopen = () => resolve(ws);
    ws.onerror = () => reject(new Error('WebSocket error'));
    ws.onclose = () => console.error('WebSocket disconnected');
    ws.onmessage = (e) => {
      if (typeof (e.data) != 'string')
        return;
      const message = JSON.parse(e.data);
      console.log(message);
      const {id, type} = message;

      let pc = peerConnectionMap[id];
      if (!pc) {
        if (type != 'offer')
          return;

        // Create PeerConnection for answer
        console.log(`Answering to ${id}`);
        pc = createPeerConnection(ws, id);
      }

      switch (type) {
      case 'offer':
      case 'answer':
        pc.setRemoteDescription({
            sdp : message.description,
            type : message.type,
          }).then(() => {
          if (type == 'offer') {
            // Send answer
            sendLocalDescription(ws, id, pc, 'answer');
          }
        });
        break;

      case 'candidate':
        pc.addIceCandidate({
          candidate : message.candidate,
          sdpMid : message.mid,
        });
        break;
      }
    }
  });
}

function offerPeerConnection(ws, id) {
  // Create PeerConnection
  console.log(`Offering to ${id}`);
  pc = createPeerConnection(ws, id);

  // Create DataChannel
  const label = "test";
  console.log(`Creating DataChannel with label "${label}"`);
  const dc = pc.createDataChannel(label);
  setupDataChannel(dc, id);

  // Send offer
  sendLocalDescription(ws, id, pc, 'offer');
}

// Create and setup a PeerConnection
function createPeerConnection(ws, id) {
  const pc = new RTCPeerConnection(config);
  pc.oniceconnectionstatechange = () => console.log(`Connection state: ${pc.iceConnectionState}`);
  pc.onicegatheringstatechange = () => console.log(`Gathering state: ${pc.iceGatheringState}`);
  pc.onicecandidate = (e) => {
    if (e.candidate && e.candidate.candidate) {
      // Send candidate
      sendLocalCandidate(ws, id, e.candidate);
    }
  };
  pc.ondatachannel = (e) => {
    const dc = e.channel;
    console.log(`"DataChannel from ${id} received with label "${dc.label}"`);
    setupDataChannel(dc, id);

    dc.send(`Hello from ${localId}`);

    sendMsg.disabled = false;
    sendBtn.disabled = false;
    sendBtn.onclick = () => dc.send(sendMsg.value);
  };

  peerConnectionMap[id] = pc;
  return pc;
}

// Setup a DataChannel
function setupDataChannel(dc, id) {
  dc.onopen = () => {
    console.log(`DataChannel from ${id} open`);

    sendMsg.disabled = false;
    sendBtn.disabled = false;
    sendBtn.onclick = () => dc.send(sendMsg.value);
  };
  dc.onclose = () => { console.log(`DataChannel from ${id} closed`); };
  dc.onmessage = (e) => {
    if (typeof (e.data) != 'string')
      return;
    console.log(`Message from ${id} received: ${e.data}`);
    document.body.appendChild(document.createTextNode(e.data));
  };

  dataChannelMap[id] = dc;
  return dc;
}

function sendLocalDescription(ws, id, pc, type) {
  (type == 'offer' ? pc.createOffer() : pc.createAnswer())
      .then((desc) => pc.setLocalDescription(desc))
      .then(() => {
        const {sdp, type} = pc.localDescription;
        ws.send(JSON.stringify({
          id,
          type,
          description : sdp,
        }));
      });
}

function sendLocalCandidate(ws, id, cand) {
  const {candidate, sdpMid} = cand;
  ws.send(JSON.stringify({
    id,
    type : 'candidate',
    candidate,
    mid : sdpMid,
  }));
}

// Helper function to generate a random ID
function randomId(length) {
  const characters = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
  const pickRandom = () => characters.charAt(Math.floor(Math.random() * characters.length));
  return [...Array(length) ].map(pickRandom).join('');
}

});
