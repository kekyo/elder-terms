// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

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
// These peers accept TCP only: a scan must not require TLS or authentication.
for (const port of [80, 443, 445, 990]) {
  const peer = createServer((socket) => {
    socket.on('error', () => {});
    socket.end();
  });
  await new Promise<void>((resolve, reject) => {
    peer.once('error', reject);
    peer.listen(port, '0.0.0.0', resolve);
  });
}
await new Promise<void>((resolve, reject) => {
  server.once('error', reject);
  server.listen(23, '0.0.0.0', resolve);
});
writeFileSync('/run/ip-scan-server-ready', 'ready\n');
