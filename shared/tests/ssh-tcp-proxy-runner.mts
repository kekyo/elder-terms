// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withSshGateway } from './ssh-proxy-test-helper.mjs';

const directory = await mkdtemp(join(tmpdir(), 'elder-tcp-proxy-'));
const servers = [0, 1].map((index) => createServer({ allowHalfOpen: true }, (socket) => {
  let received = '';
  socket.on('data', (data) => { received += data.toString(); });
  socket.on('end', () => socket.end(`${index}:${received}`));
}));
try {
  const ports: number[] = [];
  for (const server of servers) {
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    assert.ok(address && typeof address !== 'string');
    ports.push(address.port);
  }
  await withSshGateway(process.argv[3]!, directory, async (gateway) => {
    const child = spawn(process.argv[2]!, [String(gateway.port), ...ports.map(String), gateway.knownHosts], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.on('data', (bytes) => { output += bytes; });
    child.stderr.on('data', (bytes) => { output += bytes; });
    const [status] = await once(child, 'exit');
    assert.equal(status, 0, output);
    assert.deepEqual(gateway.requests.map((request) => request.port), [ports[0], ports[1], ports[0]]);
    assert.ok(gateway.requests.every((request) => request.host === 'proxy-test.invalid'));
    process.stdout.write(output);
  });
} finally {
  for (const server of servers) await new Promise<void>((resolve) => server.close(() => resolve()));
  await rm(directory, { recursive: true, force: true });
}
