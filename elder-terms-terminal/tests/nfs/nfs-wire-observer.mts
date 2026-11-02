// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import {
  createConnection,
  createServer,
  type Server,
  type Socket,
} from 'node:net';
import { once } from 'node:events';

/** RPC request information observed on the actual TCP connection. */
export interface NfsObservedCall {
  /** ONC RPC program number. */
  program: number;
  /** ONC RPC program version. */
  version: number;
  /** Procedure number. */
  procedure: number;
  /** NFSv4 COMPOUND minor version, if this is a COMPOUND request. */
  minor: number | undefined;
}

/** Test-only transparent proxy; independent of libnfs diagnostic APIs. */
export interface NfsWireObserver {
  /** Captured requests; the runner clears this before starting each probe. */
  calls: NfsObservedCall[];
  /** Malformed records encountered by the observer. */
  errors: string[];
  /** Rejects only MOUNT EXPORT while allowing known-export MNT calls through. */
  rejectExports: (enabled: boolean) => void;
  /** Closes listeners and sockets, including interrupted clients. */
  close: () => Promise<void>;
}

/**
 * Observes NFS and MOUNT requests, including fragmented RPC records.
 * @returns Observer listening at 2049 and 20048, forwarding to 22049 and 22048.
 * @remarks Runs in the test's private network namespace. Reserved-source-port
 * tests bypass this proxy so forwarding cannot change their authentication.
 */
export const createNfsWireObserver = async (): Promise<NfsWireObserver> => {
  const calls: NfsObservedCall[] = [];
  const errors: string[] = [];
  const sockets = new Set<Socket>();
  const servers: Server[] = [];
  let rejectExports = false;
  const close = async (): Promise<void> => {
    for (const socket of sockets) socket.destroy();
    for (const server of servers)
      await new Promise<void>((done) => server.close(() => done()));
  };
  try {
    for (const [port, upstreamPort] of [
      [2049, 22049],
      [20048, 22048],
    ]) {
      const server = createServer((client) => {
        const upstream = createConnection({
          host: '127.0.0.1',
          port: upstreamPort,
        });
        for (const socket of [client, upstream]) {
          sockets.add(socket);
          socket.on('close', () => sockets.delete(socket));
          socket.on('error', () => {
            client.destroy();
            upstream.destroy();
          });
        }
        client.on('close', () => upstream.destroy());
        upstream.on('close', () => client.destroy());
        let pending = Buffer.alloc(0);
        let fragments: Buffer[] = [];
        client.on('data', (bytes: Buffer) => {
          try {
            pending = Buffer.concat([pending, bytes]);
            while (pending.length >= 4) {
              const marker = pending.readUInt32BE(0);
              const size = marker & 0x7fffffff;
              if (size > 16 * 1024 * 1024)
                throw new Error('RPC record is too large');
              if (pending.length < size + 4) break;
              fragments.push(pending.subarray(4, size + 4));
              pending = pending.subarray(size + 4);
              if ((marker & 0x80000000) === 0) continue;
              const record = Buffer.concat(fragments);
              fragments = [];
              if (
                record.length < 24 ||
                record.readUInt32BE(4) !== 0 ||
                record.readUInt32BE(8) !== 2
              )
                throw new Error('Invalid ONC RPC call');
              const program = record.readUInt32BE(12);
              const version = record.readUInt32BE(16);
              const procedure = record.readUInt32BE(20);
              let minor: number | undefined;
              if (program === 100003 && version === 4 && procedure === 1) {
                let offset = 24;
                for (let auth = 0; auth < 2; auth++)
                  offset += 8 + ((record.readUInt32BE(offset + 4) + 3) & ~3);
                offset += 4 + ((record.readUInt32BE(offset) + 3) & ~3);
                minor = record.readUInt32BE(offset);
              }
              calls.push({ program, version, procedure, minor });
              if (rejectExports && program === 100005 && procedure === 5) {
                const reply = Buffer.alloc(28);
                reply.writeUInt32BE(0x80000018, 0);
                reply.writeUInt32BE(record.readUInt32BE(0), 4);
                reply.writeUInt32BE(1, 8);
                reply.writeUInt32BE(5, 24); // RPC accepted, SYSTEM_ERR.
                client.write(reply);
              } else {
                const header = Buffer.alloc(4);
                header.writeUInt32BE((record.length | 0x80000000) >>> 0);
                upstream.write(Buffer.concat([header, record]));
              }
            }
          } catch (error) {
            errors.push(String(error));
            client.destroy();
            upstream.destroy();
          }
        });
        upstream.pipe(client);
      });
      servers.push(server);
      server.listen(port, '127.0.0.1');
      await once(server, 'listening');
    }
    return {
      calls,
      errors,
      close,
      rejectExports: (enabled) => {
        rejectExports = enabled;
      },
    };
  } catch (error) {
    await close();
    throw error;
  }
};
