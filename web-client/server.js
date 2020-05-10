const http = require('http');

let connectionMetadata;
http.createServer(function(req, res) {
  console.log(req.method.toUpperCase(), req.url);
  if (req.method === 'POST') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      connectionMetadata = body;
      res.writeHead(200);
      res.end();
    });
    return;
  }
  if (req.method === 'GET') {
    res.writeHead(200, {'Content-Type': 'text/plain'});
    res.end(connectionMetadata);
    return;
  }
  if (req.method === 'OPTIONS') {
    res.writeHead(200, {
      'Access-Control-Allow-Origin': '*',
    });
    res.end(connectionMetadata);
    return;
  }
  console.error('unknown method: ' + req.method);
}).listen(8000);
