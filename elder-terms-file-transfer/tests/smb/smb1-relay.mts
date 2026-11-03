import { createConnection, createServer, type Socket } from 'node:net';
import { once } from 'node:events';

let armed = false;
let mode = '';
const sockets = new Set<Socket>();
process.stdin.setEncoding('utf8').on('data', (text: string) => {
  mode = text.trim();
  armed = true;
  console.log('ARMED');
});
const listener = createServer((client) => {
  const upstream = createConnection({ host: '127.0.0.1', port: 1445 });
  for (const socket of [client, upstream]) {
    sockets.add(socket);
    socket.on('error', () => {
      /* Fault cases deliberately break connections. */
    });
    socket.on('close', () => sockets.delete(socket));
  }
  client.on('close', () => upstream.destroy());
  upstream.on('close', () => client.destroy());
  let pending = Buffer.alloc(0);
  let held = false;
  let reported = false;
  client.on('data', (bytes) => {
    pending = Buffer.concat([pending, Buffer.from(bytes)]);
    while (
      pending.length >= 4 &&
      pending.length >= 4 + pending.readUInt32BE(0)
    ) {
      const size = 4 + pending.readUInt32BE(0);
      const packet = pending.subarray(0, size);
      pending = pending.subarray(size);
      if (armed && packet.length > 8 && packet[8] === 0x2e) {
        armed = false;
        held = true;
      }
      upstream.write(packet);
    }
  });
  upstream.on('data', (bytes) => {
    if (!held) {
      client.write(bytes);
      return;
    }
    if (reported) return;
    reported = true;
    console.log('HELD');
    if (mode === 'disconnect') client.destroy();
  });
});
listener.listen(445, '127.0.0.1');
await once(listener, 'listening');
console.log('READY relay');
process.stdin.on('end', () => {
  for (const socket of sockets) socket.destroy();
  listener.close();
});
