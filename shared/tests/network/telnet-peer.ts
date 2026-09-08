import { createServer } from 'node:net';
import { writeFileSync } from 'node:fs';

// A standard-port TCP peer is enough to exercise the application's TELNET
// handshake and terminal receive path; negotiation bytes are not echoed.
const server = createServer((socket) => {
  let received = '';
  socket.on('error', () => {});
  socket.write('MULTICAST CONNECTION VERIFIED\r\n');
  socket.on('data', (bytes) => {
    received += bytes.toString('utf8');
    if (received.includes('CONFIRMED SCANNED NAME')) {
      writeFileSync('/evidence/peer-response.txt', 'CONFIRMED SCANNED NAME\n');
    }
  });
});
server.listen(23, '0.0.0.0', () => {
  writeFileSync('/run/ip-scan-server-ready', 'ready\n');
});
