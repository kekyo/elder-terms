// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer, type Socket } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withSshGateway } from '../../../shared/tests/ssh-proxy-test-helper.mjs';

const run = async (
  command: string,
  args: string[],
  env: NodeJS.ProcessEnv
): Promise<void> => {
  const child = spawn(command, args, {
    env,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  child.stdout.on('data', (bytes) => {
    output += bytes;
  });
  child.stderr.on('data', (bytes) => {
    output += bytes;
  });
  const [status] = await once(child, 'exit');
  assert.equal(status, 0, output);
  process.stdout.write(output);
};
if (!process.argv.includes('--isolated')) {
  await run(
    'podman',
    [
      'unshare',
      'unshare',
      '--net',
      process.execPath,
      fileURLToPath(import.meta.url),
      ...process.argv.slice(2),
      '--isolated',
    ],
    process.env
  );
} else {
  await run('ip', ['link', 'set', 'lo', 'up'], process.env);
  const directory = await mkdtemp(join(tmpdir(), 'elder-nfs-proxy-discovery-'));
  try {
    for (const mode of ['ipv6-mapping', 'invalid-mapping']) {
      const sockets = new Set<Socket>();
      const services = [0, 1].map(() =>
        createServer((socket) => {
          sockets.add(socket);
          socket.on('error', () => {});
        })
      );
      const ports: number[] = [];
      for (const service of services) {
        service.listen(0, '127.0.0.2');
        await once(service, 'listening');
        const address = service.address();
        assert.ok(address && typeof address !== 'string');
        ports.push(address.port);
      }
      const calls: { protocol: number; program: number; version: number }[] =
        [];
      const mapper = createServer((socket) => {
        sockets.add(socket);
        socket.on('error', () => {});
        let pending = Buffer.alloc(0);
        socket.on('data', (data) => {
          assert.ok(Buffer.isBuffer(data));
          pending = Buffer.concat([pending, data]);
          while (
            pending.length >= 4 &&
            pending.length >= 4 + (pending.readUInt32BE(0) & 0x7fffffff)
          ) {
            const size = pending.readUInt32BE(0) & 0x7fffffff;
            const request = pending.subarray(4, size + 4);
            pending = pending.subarray(size + 4);
            assert.equal(request.readUInt32BE(12), 100000);
            assert.equal(request.readUInt32BE(20), 3);
            const protocol = request.readUInt32BE(16);
            let offset = 24;
            for (let auth = 0; auth < 2; auth++)
              offset += 8 + ((request.readUInt32BE(offset + 4) + 3) & ~3);
            const program = request.readUInt32BE(offset);
            const version = request.readUInt32BE(offset + 4);
            calls.push({ protocol, program, version });
            let payload: Buffer;
            if (protocol === 2) {
              payload = Buffer.alloc(4);
              payload.writeUInt32BE(mode === 'invalid-mapping' ? 65536 : 0);
            } else {
              const length = request.readUInt32BE(offset + 8);
              assert.equal(
                request.subarray(offset + 12, offset + 12 + length).toString(),
                'tcp6'
              );
              const port = ports[program === 100003 ? 0 : 1]!;
              const address = Buffer.from(
                `2001:db8::1.${port >> 8}.${port & 255}`
              );
              payload = Buffer.alloc(4 + ((address.length + 3) & ~3));
              payload.writeUInt32BE(address.length);
              address.copy(payload, 4);
            }
            const reply = Buffer.alloc(28);
            reply.writeUInt32BE(0x80000000 + 24 + payload.length);
            reply.writeUInt32BE(request.readUInt32BE(0), 4);
            reply.writeUInt32BE(1, 8);
            socket.write(Buffer.concat([reply, payload]));
          }
        });
      });
      mapper.listen(111, '127.0.0.2');
      await once(mapper, 'listening');
      try {
        await withSshGateway(
          process.argv[3]!,
          directory,
          async (gateway) => {
            await run(process.argv[2]!, ['3', mode], {
              ...process.env,
              ELDER_NFS_PROXY_PORT: String(gateway.port),
              ELDER_NFS_PROXY_KNOWN_HOSTS: gateway.knownHosts,
            });
            assert.deepEqual(
              gateway.requests.map((request) => request.port),
              mode === 'invalid-mapping' ? [111] : [111, ...ports]
            );
            assert.ok(
              gateway.requests.every(
                (request) => request.host === 'proxy-test.invalid'
              )
            );
            assert.deepEqual(
              calls,
              mode === 'invalid-mapping'
                ? [{ protocol: 2, program: 100003, version: 3 }]
                : [
                    { protocol: 2, program: 100003, version: 3 },
                    { protocol: 3, program: 100003, version: 3 },
                    { protocol: 2, program: 100005, version: 3 },
                    { protocol: 3, program: 100005, version: 3 },
                  ]
            );
          },
          '127.0.0.2'
        );
      } finally {
        for (const socket of sockets) socket.destroy();
        for (const service of [mapper, ...services])
          await new Promise<void>((resolve) => service.close(() => resolve()));
      }
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}
