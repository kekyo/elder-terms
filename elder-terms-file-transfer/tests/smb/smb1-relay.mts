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
  let response = Buffer.alloc(0);
  client.on('data', (bytes) => {
    pending = Buffer.concat([pending, Buffer.from(bytes)]);
    while (
      pending.length >= 4 &&
      pending.length >= 4 + pending.readUInt32BE(0)
    ) {
      const size = 4 + pending.readUInt32BE(0);
      let packet = pending.subarray(0, size);
      pending = pending.subarray(size);
      const operation = mode === 'partial-shares' ? 0x25 : mode === 'connect-timeout' ? 0x72 : mode.endsWith('create')
        ? 0x00
        : mode.endsWith('write')
          ? 0x2f
          : mode.endsWith('commit')
            ? 0x07
            : mode === 'invalid-name'
              ? 0x32
              : 0x2e;
      // NetShareEnum is a request PDU; the preceding bind uses the same SMB command.
      const shareRequest = mode !== 'partial-shares' || (packet.length > 111 && packet[90] === 0 && packet.readUInt16LE(110) === 15);
      if (armed && packet.length > 8 && packet[8] === operation && shareRequest) {
        armed = false;
        held = true;
        if (mode === 'short-write') {
          // MS-CIFS WRITE_ANDX: send only the accepted prefix to Samba so the
          // real response advances the file by fewer bytes than requested.
          const start = 4 + packet.readUInt16LE(59);
          const accepted = Math.min(4093, packet.readUInt16LE(57));
          packet = Buffer.from(packet.subarray(0, start + accepted));
          packet.writeUInt32BE(packet.length - 4, 0);
          packet.writeUInt16LE(accepted, 57);
          packet.writeUInt16LE(packet.length - 67, 65);
        }
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
    if (['short-write', 'zero-write', 'invalid-name', 'partial-shares'].includes(mode)) {
      response = Buffer.concat([response, Buffer.from(bytes)]);
      if (response.length < 4 || response.length < 4 + response.readUInt32BE(0))
        return;
      if (mode === 'zero-write') response.writeUInt16LE(0, 41);
      if (mode === 'partial-shares') {
        const statusOffset = response.readUInt32BE(0);
        if (response.readUInt32LE(statusOffset) !== 0) throw new Error('NetShareEnum did not succeed before fault injection');
        response.writeUInt32LE(234, statusOffset); // ERROR_MORE_DATA, MS-SRVS NetrShareEnum.
      }
      if (mode === 'invalid-name') {
        const offset = response.indexOf(Buffer.from('before.txt', 'utf16le'));
        if (offset < 0)
          throw new Error(
            'The directory response did not contain the fixture name'
          );
        response.writeUInt16LE(0xd800, offset);
      }
      held = false;
      client.write(response);
    }
    reported = true;
    console.log('HELD');
    if (mode.startsWith('disconnect')) client.destroy();
  });
});
listener.listen(445, '127.0.0.1');
await once(listener, 'listening');
console.log('READY relay');
process.stdin.on('end', () => {
  for (const socket of sockets) socket.destroy();
  listener.close();
});
