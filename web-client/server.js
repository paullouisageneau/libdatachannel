const http = require('http');



const connection = {
    "description":"v=0\r\no=- 9134460598269614011 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\na=msid-semantic: WMS\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:xlQY\r\na=ice-pwd:STMZXrUH+JPpmy86lz5Tp0YB\r\na=ice-options:trickle\r\na=fingerprint:sha-256 80:4C:CB:B5:45:89:0C:51:0A:2A:E5:AC:96:A7:53:F3:42:2B:F6:B8:1D:B0:CE:44:9F:0B:86:FC:B0:BD:94:F7\r\na=setup:actpass\r\na=mid:0\r\na=sctp-port:5000\r\na=max-message-size:262144\r\n",
    "candidate":"candidate:269456666 1 udp 2113937151 10.0.1.185 58326 typ host generation 0 ufrag xlQY network-cost 999"
};

console.log('http://localhost:8000/answerer.html?connection=' + Buffer.from(JSON.stringify(connection)).toString('base64'));
http.createServer(function(req, res){
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify());
}).listen(3030);