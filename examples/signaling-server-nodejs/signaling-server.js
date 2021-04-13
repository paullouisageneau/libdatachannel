/*
 * libdatachannel example web server
 * Copyright (C) 2020 Lara Mackey
 * Copyright (C) 2020 Paul-Louis Ageneau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

const http = require('http');
const websocket = require('websocket');

const clients = {};

const httpServer = http.createServer((req, res) => {
  console.log(`${req.method.toUpperCase()} ${req.url}`);

  const respond = (code, data, contentType = 'text/plain') => {
    res.writeHead(code, {
      'Content-Type' : contentType,
      'Access-Control-Allow-Origin' : '*',
    });
    res.end(data);
  };

  respond(404, 'Not Found');
});

const wsServer = new websocket.server({httpServer});
wsServer.on('request', (req) => {
  console.log(`WS  ${req.resource}`);

  const {path} = req.resourceURL;
  const splitted = path.split('/');
  splitted.shift();
  const id = splitted[0];

  const conn = req.accept(null, req.origin);
  conn.on('message', (data) => {
    if (data.type === 'utf8') {
      console.log(`Client ${id} << ${data.utf8Data}`);

      const message = JSON.parse(data.utf8Data);
      const destId = message.id;
      const dest = clients[destId];
      if (dest) {
        message.id = id;
        const data = JSON.stringify(message);
        console.log(`Client ${destId} >> ${data}`);
        dest.send(data);
      } else {
        console.error(`Client ${destId} not found`);
      }
    }
  });
  conn.on('close', () => {
    delete clients[id];
    console.error(`Client ${id} disconnected`);
  });

  clients[id] = conn;
});

const endpoint = process.env.PORT || '8000';
const splitted = endpoint.split(':');
const port = splitted.pop();
const hostname = splitted.join(':') || '127.0.0.1';

httpServer.listen(port, hostname,
                  () => { console.log(`Server listening on ${hostname}:${port}`); });
