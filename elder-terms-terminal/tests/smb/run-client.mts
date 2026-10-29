import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { once } from 'node:events';
import { createConnection, createServer, type Socket } from 'node:net';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const name = `elder-smb-client-${process.pid}`;
const env = {
  ...process.env,
  ELDER_SMB_PASSWORD: randomBytes(24).toString('hex'),
};
const podman = (args: string[]): string =>
  execFileSync('podman', args, { env, encoding: 'utf8' });

const runFault = async (
  port: string,
  mode: string,
  driver: string
): Promise<void> => {
  let armed = false;
  let injected = false;
  const sockets = new Set<Socket>();
  let child: ReturnType<typeof spawn>;
  const listener = createServer((client) => {
    const upstream = createConnection({
      host: '127.0.0.1',
      port: Number(port),
    });
    sockets.add(client);
    sockets.add(upstream);
    for (const socket of [client, upstream]) {
      socket.on('error', () => {
        /* Abrupt disconnect is part of the fixture. */
      });
      socket.on('close', () => sockets.delete(socket));
    }
    client.on('close', () => upstream.destroy());
    upstream.on('close', () => client.destroy());
    let buffered = Buffer.alloc(0);
    let suppressed = false;
    let triggered = false;
    client.on('data', (bytes) => {
      buffered = Buffer.concat([buffered, Buffer.from(bytes)]);
      while (
        buffered.length >= 4 &&
        buffered.length >= 4 + buffered.readUInt32BE(0)
      ) {
        const length = 4 + buffered.readUInt32BE(0);
        const packet = buffered.subarray(0, length);
        buffered = buffered.subarray(length);
        const command = packet.length >= 18 ? packet.readUInt16LE(16) : -1;
        const target = mode.endsWith('-read')
          ? command === 8
          : mode === 'cancel-write'
            ? command === 9
            : command === 5 &&
              packet.length >= 108 &&
              packet.readUInt32LE(104) === 2;
        if (armed && !injected && target) {
          injected = true;
          suppressed = true;
        }
        upstream.write(packet);
      }
    });
    upstream.on('data', (bytes) => {
      if (!suppressed) {
        client.write(bytes);
        return;
      }
      if (triggered) return;
      triggered = true;
      if (mode.startsWith('disconnect-')) client.destroy();
      else if (mode.startsWith('cancel-')) child.stdin!.write('c');
      // The server has handled the request; the reply stays withheld.
    });
  });
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const address = listener.address();
  assert.ok(address && typeof address !== 'string');
  child = spawn(
    driver,
    ['127.0.0.1', 'probe', 'elder', String(address.port), mode],
    {
      env,
      stdio: ['pipe', 'pipe', 'pipe'],
    }
  );
  let output = '';
  child.stdout!.setEncoding('utf8').on('data', (text: string) => {
    output += text;
    if (!armed && output.includes(`READY SMB fault ${mode}`)) {
      armed = true;
      child.stdin!.write('r');
    }
  });
  child.stderr!.setEncoding('utf8').on('data', (text: string) => {
    output += text;
  });
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 30_000);
  try {
    const [status] = await once(child, 'exit');
    assert.equal(status, 0, output.slice(-2000));
    assert.ok(injected, `${mode} did not inject the SMB fault`);
    assert.match(
      output,
      new RegExp(`PASS SMB fault ${mode} reconnect/content/cleanup`)
    );
    console.log(`${mode}: PASS`);
  } finally {
    clearTimeout(watchdog);
    for (const socket of sockets) socket.destroy();
    await new Promise<void>((done) => listener.close(() => done()));
  }
};
try {
  podman(['image', 'exists', 'localhost/elder-plan12-samba']);
} catch {
  const architecture = process.arch === 'x64' ? 'amd64' : process.arch;
  podman([
    'build',
    '--platform',
    `linux/${architecture}`,
    '-t',
    'localhost/elder-plan12-samba',
    '-f',
    resolve(directory, 'Containerfile'),
    directory,
  ]);
}
const server = spawn(
  'podman',
  [
    'run',
    '--rm',
    '--name',
    name,
    '-p',
    '127.0.0.1::445',
    '--env',
    'ELDER_SMB_PASSWORD',
    '-v',
    `${resolve(directory, 'samba-entry.sh')}:/entry.sh:ro`,
    'localhost/elder-plan12-samba',
    'sh',
    '/entry.sh',
  ],
  { env, stdio: ['ignore', 'pipe', 'pipe'] }
);
try {
  await new Promise<void>((ready, reject) => {
    let output = '';
    let running = false;
    const timeout = setTimeout(
      () => reject(new Error(`Samba did not become ready: ${output}`)),
      30000
    );
    const collect = (chunk: Buffer) => {
      if (running) return;
      output += chunk;
      if (output.includes('ready to serve connections')) {
        running = true;
        clearTimeout(timeout);
        ready();
      }
    };
    server.stdout.on('data', collect);
    server.stderr.on('data', collect);
    server.on('error', reject);
    server.on('exit', (code) => {
      clearTimeout(timeout);
      reject(new Error(`Samba exited ${code}: ${output}`));
    });
  });
  const port = podman(['port', name, '445/tcp']).trim().split(':').pop()!;
  podman([
    'exec',
    name,
    'sh',
    '-c',
    "printf 'SMB fixture content\\n' >/share/sample.txt",
  ]);
  for (const share of ['probe', 'encrypted', 'readonly']) {
    const child = spawn(
      process.argv[2] ??
        process.env.ELDER_SMB_CLIENT_DRIVER ??
        resolve(root, '.build/smb-probe/smb-client-test'),
      ['localhost', share, 'elder', port],
      {
        env,
        stdio: ['ignore', 'pipe', 'pipe'],
      }
    );
    let output = '';
    child.stdout.on('data', (chunk) => {
      output += chunk;
    });
    child.stderr.on('data', (chunk) => {
      output += chunk;
    });
    const [status] = await once(child, 'exit');
    assert.equal(status, 0, output);
    assert.match(output, /PASS SMB client/);
    console.log(`${share}: ${output.trim()}`);
  }
  if (process.argv[3])
    for (const mode of process.env.ELDER_SMB_FAULT_MODES?.split(',') ?? [
      'cancel-read',
      'disconnect-read',
      'timeout-read',
      'cancel-write',
      'cancel-create',
      'disconnect-create',
    ])
      await runFault(port, mode, process.argv[3]);
  assert.equal(
    podman(['exec', name, 'cat', '/outside/sentinel.txt']),
    'outside sentinel\n'
  );
} finally {
  podman(['rm', '-f', name]);
}
