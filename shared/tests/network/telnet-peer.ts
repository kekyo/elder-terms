import { createServer } from 'node:net';
import { writeFileSync } from 'node:fs';

// A standard-port TCP peer is enough to exercise the application's TELNET
// handshake and terminal receive path; negotiation bytes are not echoed.
let completedConnections = 0;
const server = createServer((socket) => {
  let received = '';
  let completed = false;
  socket.on('error', () => {});
  socket.write('MULTICAST CONNECTION VERIFIED\r\n');
  socket.on('data', (bytes) => {
    received += bytes.toString('utf8');
    if (!completed && received.includes('CONFIRMED SCANNED NAME')) {
      completed = true;
      writeFileSync('/evidence/peer-response.txt', 'CONFIRMED SCANNED NAME\n');
      writeFileSync(
        '/evidence/peer-connections.txt',
        String(++completedConnections)
      );
      socket.end();
    }
  });
});
server.listen(23, '0.0.0.0', () => {
  writeFileSync('/run/ip-scan-server-ready', 'ready\n');
});
