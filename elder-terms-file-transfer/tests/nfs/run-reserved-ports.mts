// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { once } from 'node:events';
import { createServer, type Socket } from 'node:net';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';

const execute = promisify(execFile);
if (!process.argv.includes('--isolated')) {
  const { stdout } = await execute(
    'podman',
    [
      'unshare',
      'unshare',
      '--net',
      process.execPath,
      fileURLToPath(import.meta.url),
      '--isolated',
    ],
    { timeout: 120000 }
  );
  process.stdout.write(stdout);
} else {
  await execute('ip', ['link', 'set', 'lo', 'up']);
  const driver =
    process.env.ELDER_NFS_PROBE ??
    fileURLToPath(new URL('../../../.build/nfs/nfs-probe', import.meta.url));
  for (const host of ['127.0.0.1', '::1']) {
    for (const version of [2, 3, 4]) {
      for (const [required, privileged] of [
        [true, false],
        [true, true],
        [false, false],
      ]) {
        const sockets = new Set<Socket>();
        const sourcePorts: number[] = [];
        let requestBytes = 0;
        let child: ReturnType<typeof spawn>;
        const server = createServer((socket) => {
          sockets.add(socket);
          // A rejected connection may already be closed when Node accepts it.
          if (socket.remotePort !== undefined)
            sourcePorts.push(socket.remotePort);
          socket.on('error', () => {
            /* The probe deliberately aborts its session. */
          });
          socket.on('close', () => sockets.delete(socket));
          socket.on('data', (bytes) => {
            requestBytes += bytes.length;
            child.kill('SIGUSR1');
          });
        });
        server.listen(0, host);
        await once(server, 'listening');
        const address = server.address();
        assert.ok(address && typeof address !== 'string');
        const mode = required ? 'reserved-required' : 'reserved-optional';
        const args = [
          '--fault',
          String(version),
          host,
          String(address.port),
          mode,
        ];
        child = privileged
          ? spawn(driver, args)
          : spawn('setpriv', [
              '--bounding-set=-net_bind_service',
              driver,
              ...args,
            ]);
        let output = '';
        child.stdout!.on('data', (bytes) => {
          output += bytes;
        });
        child.stderr!.on('data', (bytes) => {
          output += bytes;
        });
        const watchdog = setTimeout(() => child.kill('SIGKILL'), 20000);
        try {
          const [status] = await once(child, 'close');
          assert.equal(status, 0, output);
          // Closing the listener waits for accepted sockets, so the assertions
          // include all request bytes sent before the probe terminated.
          await new Promise<void>((resolve) => server.close(() => resolve()));
          if (required && !privileged) {
            assert.equal(
              requestBytes,
              0,
              'Required reserved ports must prevent RPC traffic from a high source port'
            );
            assert.match(output, /reserved source port/);
          } else {
            assert.ok(
              requestBytes > 0,
              'An allowed connection must send a real RPC request'
            );
            assert.ok(sourcePorts.length > 0);
          }
          assert.ok(
            sourcePorts.every((port) =>
              privileged ? port > 0 && port < 1024 : port >= 1024
            ),
            JSON.stringify({
              sourcePorts,
              required,
              privileged,
              host,
              version,
              output,
            })
          );
          process.stdout.write(
            `PASS source port NFS v${version} ${host} required=${required} privileged=${privileged}\n`
          );
        } finally {
          clearTimeout(watchdog);
          for (const socket of sockets) socket.destroy();
          if (server.listening)
            await new Promise<void>((resolve) => server.close(() => resolve()));
          if (child.exitCode === null && child.signalCode === null)
            child.kill('SIGKILL');
        }
      }
    }
  }
}
