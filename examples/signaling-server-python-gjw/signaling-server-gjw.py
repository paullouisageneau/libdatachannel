#!/usr/bin/env python
#
# Python signaling server example for libdatachannel
# Copyright (c) 2020 Paul-Louis Ageneau
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

import sys
import ssl
import json
import asyncio
import logging
import websockets


logger = logging.getLogger('websockets')
logger.setLevel(logging.DEBUG)
logger.addHandler(logging.StreamHandler(sys.stdout))

# bookkeeping the clients' ids and the websocket instance
clients = {}

# bookkeeping the clients' ids and it's peer's id
peermap = {}

# basic APIs:
# join/{clientid}
# indicates a client has login to the server and may want to
# connect to a peer later on, we don't handle Dos attach for now!
#
# offer/{peerid}
# indicates a client attempts to negotiate a connection to a peer
# reject if the peer is alread in connection or the peer hasn't joined
#
# TODO

async def sendResponse(websocket, id, type):
    message = {}
    message["id"] = id
    message["type"] = type

    print(f"Client id: {id}, type: {type}")

    data = json.dumps(message)

    await websocket.send(data)

async def handle_websocket(websocket, path):
    client_id = None
    destination_id = None
    try:
        splitted = path.split('/')
        splitted.pop(0)

        request = splitted.pop(0)

        client_id = None

        if request == "join":
            client_id = splitted.pop(0)
        else:
            raise RuntimeError("Not implemented yet")

        if not client_id:
            raise RuntimeError("Missing client ID")

        if client_id in clients:
            raise RuntimeError("Duplicated request for join")

        clients[client_id] = websocket

        while True:
            data = await websocket.recv()
            print('Client {} << {}'.format(client_id, data))

            message = json.loads(data)

            destination_id = message['id']
            destination_websocket = clients.get(destination_id)

            request = message['type']

            if not destination_websocket:
                print('Peer {} not found'.format(destination_id))
                await sendResponse(websocket, client_id, "useroffline")
                continue

            # reject multiple request for peerconnection
            if request == "offer" and (client_id in peermap or destination_id in peermap):
                print('Client {} already in peerconnection'.format(client_id))
                await sendResponse(websocket, client_id, "userbusy")
                continue

            if request == "leave":
                print('Client {} requests to leave'.format(client_id))
                await sendResponse(destination_websocket, client_id, "leave")
                break

            # map the peer id to this client if necessary
            # offer/answer
            peermap[client_id] = destination_id

            print("Sending message to {}".format(destination_id))

            message['id'] = client_id
            data = json.dumps(message)

            print('Client {} >> {}'.format(destination_id, data))

            await destination_websocket.send(data)

    except Exception as e:
        print(e)

    finally:
        if client_id:
            del clients[client_id]

            if client_id in peermap:
                del peermap[client_id]

        if destination_id:
            destination_websocket = clients.get(destination_id)

            if destination_websocket:
                await sendResponse(websocket, client_id, "leave")

            if destination_id in peermap:
                del peermap[destination_id]

        print('Client {} disconnected'.format(client_id))


async def main():
    # Usage: ./server.py [[host:]port] [SSL certificate file]
    endpoint_or_port = sys.argv[1] if len(sys.argv) > 1 else "8000"
    ssl_cert = sys.argv[2] if len(sys.argv) > 2 else None

    endpoint = endpoint_or_port if ':' in endpoint_or_port else "127.0.0.1:" + endpoint_or_port

    if ssl_cert:
        ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_context.load_cert_chain(ssl_cert)
    else:
        ssl_context = None

    print('Listening on {}'.format(endpoint))
    host, port = endpoint.rsplit(':', 1)

    server = await websockets.serve(handle_websocket, host, int(port), ssl=ssl_context)
    await server.wait_closed()


if __name__ == '__main__':
    asyncio.run(main())
