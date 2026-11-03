// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createServer, type Socket } from 'node:net';
import { fileURLToPath } from 'node:url';

const driver =
  process.env.ELDER_NFS_PROBE ??
  fileURLToPath(new URL('../../../.build/nfs/nfs-probe', import.meta.url));
for (const version of [2, 3, 4]) {
  for (const mode of ['cancel', 'timeout', 'disconnect']) {
    const sockets = new Set<Socket>();
    let faultObserved = false;
    let child: ReturnType<typeof spawn>;
    const server = createServer((socket) => {
      sockets.add(socket);
      socket.on('error', () => {
        /* Intentional transport failure. */
      });
      socket.once('data', () => {
        faultObserved = true;
        if (mode === 'cancel') child.kill('SIGUSR1');
        else if (mode === 'disconnect') socket.destroy();
      });
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    assert.ok(address && typeof address !== 'string');
    child = spawn(
      driver,
      ['--fault', String(version), '127.0.0.1', String(address.port), mode],
      { stdio: ['ignore', 'pipe', 'pipe'] }
    );
    let output = '';
    child.stdout!.on('data', (bytes) => {
      output += bytes;
    });
    child.stderr!.on('data', (bytes) => {
      output += bytes;
    });
    const watchdog = setTimeout(() => child.kill('SIGKILL'), 15000);
    try {
      const [status] = await once(child, 'exit');
      assert.equal(status, 0, output);
      assert.ok(
        faultObserved,
        'A real RPC request must reach the peer before injecting a failure'
      );
      assert.match(output, new RegExp(`PASS fault ${version} ${mode}`));
      process.stdout.write(output);
    } finally {
      clearTimeout(watchdog);
      for (const socket of sockets) socket.destroy();
      await new Promise<void>((resolve) => server.close(() => resolve()));
      if (child.exitCode === null && child.signalCode === null)
        child.kill('SIGKILL');
    }
  }
}
