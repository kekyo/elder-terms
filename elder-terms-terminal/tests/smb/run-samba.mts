import { spawn, execFileSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import assert from 'node:assert/strict';
import { createServer, createConnection, type Socket } from 'node:net';
import { once } from 'node:events';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const image = 'localhost/elder-plan12-samba';
const name = `elder-smb-probe-${process.pid}`;
const password = randomBytes(24).toString('hex');
const env = {
  ...process.env,
  ELDER_SMB_PASSWORD: password,
  ELDER_SMB_REPARSE_PATH: 'outside-link',
};
const podman = (args: string[]) =>
  execFileSync('podman', args, { env, encoding: 'utf8' });
const runProbe = (
  port: string,
  share: string,
  mode: string,
  expected: number,
  wrongPassword: boolean,
  evidence: RegExp
) => {
  const result = spawn(
    resolve(root, '.build/smb-probe/smb-probe'),
    ['localhost', share, 'elder', port, mode],
    {
      env: wrongPassword ? { ...env, ELDER_SMB_PASSWORD: 'incorrect' } : env,
      stdio: ['ignore', 'pipe', 'pipe'],
    }
  );
  return new Promise<void>((resolveResult, reject) => {
    let output = '';
    result.stdout.on('data', (chunk) => {
      output += chunk;
    });
    result.stderr.on('data', (chunk) => {
      output += chunk;
    });
    result.on('error', reject);
    result.on('exit', (code) => {
      console.log(`${share}/${mode}: ${output.trim()}`);
      try {
        assert.equal(code, expected);
        assert.match(output, evidence);
        resolveResult();
      } catch (error) {
        reject(error);
      }
    });
  });
};

const runTransferFault = async (port: string, mode: string): Promise<void> => {
  let armed = false;
  let injected = false;
  const sockets = new Set<Socket>();
  const listener = createServer((client) => {
    const upstream = createConnection({
      host: '127.0.0.1',
      port: Number(port),
    });
    sockets.add(client);
    sockets.add(upstream);
    for (const socket of [client, upstream]) {
      socket.on('error', (error: NodeJS.ErrnoException) => {
        assert.ok(error.code === 'ECONNRESET' || error.code === 'EPIPE');
      });
      socket.on('close', () => {
        sockets.delete(socket);
      });
    }
    client.on('close', () => upstream.destroy());
    upstream.on('close', () => client.destroy());
    let buffered = Buffer.alloc(0);
    let suppressReply = false;
    let canceled = false;
    client.on('data', (data) => {
      buffered = Buffer.concat([
        buffered,
        typeof data === 'string' ? Buffer.from(data) : data,
      ]);
      while (
        buffered.length >= 4 &&
        buffered.length >= 4 + buffered.readUInt32BE(0)
      ) {
        const length = 4 + buffered.readUInt32BE(0);
        const packet = buffered.subarray(0, length);
        buffered = buffered.subarray(length);
        const command = packet.length >= 18 ? packet.readUInt16LE(16) : -1;
        if (
          armed &&
          !injected &&
          command === (mode === 'cancel-write' ? 9 : 8)
        ) {
          injected = true;
          suppressReply = true;
        }
        upstream.write(packet);
      }
    });
    upstream.on('data', (data) => {
      if (!suppressReply) {
        client.write(data);
        return;
      }
      // The server has handled the command; withhold its reply, then interrupt.
      if (!canceled) {
        canceled = true;
        if (mode === 'disconnect-read') client.destroy();
        else child.stdin.write('c');
      }
    });
  });
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const address = listener.address();
  assert.ok(address && typeof address !== 'string');
  const child = spawn(
    resolve(root, '.build/smb-probe/smb-probe'),
    ['127.0.0.1', 'probe', 'elder', String(address.port), mode],
    {
      env,
      stdio: ['pipe', 'pipe', 'pipe'],
    }
  );
  let output = '';
  child.stdout.on('data', (chunk) => {
    output += chunk;
    if (!armed && output.includes('READY transfer fault')) {
      armed = true;
      child.stdin.write('r');
    }
  });
  child.stderr.on('data', (chunk) => {
    output += chunk;
  });
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 30000);
  try {
    const [status] = await once(child, 'exit');
    assert.equal(status, 0, output);
    assert.ok(injected);
    assert.match(output, /reconnect\/content\/owned cleanup/);
    console.log(`${mode}: ${output.trim()}`);
  } finally {
    clearTimeout(watchdog);
    for (const socket of sockets) socket.destroy();
    listener.close();
  }
};

await runProbe('0', 'unused', 'dns-cancel', 0, false, /PASS DNS cancellation/);
try {
  podman(['image', 'exists', image]);
} catch {
  execFileSync(
    'podman',
    [
      'build',
      '-t',
      image,
      '-f',
      resolve(directory, 'Containerfile'),
      directory,
    ],
    { stdio: 'inherit' }
  );
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
    image,
    'sh',
    '/entry.sh',
  ],
  { env, stdio: ['ignore', 'pipe', 'pipe'] }
);
try {
  await new Promise<void>((ready, reject) => {
    let output = '';
    const timer = setTimeout(
      () => reject(new Error(`Samba did not become ready: ${output}`)),
      30000
    );
    const collect = (chunk: Buffer) => {
      output += chunk;
      if (output.includes('ready to serve connections')) {
        clearTimeout(timer);
        ready();
      }
    };
    server.stdout.on('data', collect);
    server.stderr.on('data', collect);
    server.on('error', reject);
    server.on('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`Samba exited ${code}: ${output}`));
    });
  });
  const port = podman(['port', name, '445/tcp']).trim().split(':').pop()!;
  await runProbe(port, 'probe', 'basic', 0, false, /PASS list/);
  await runProbe(port, 'probe', 'smb2', 0, false, /dialect=528/);
  await runProbe(port, 'encrypted', 'encrypt', 0, false, /PASS list/);
  await runProbe(
    port,
    'probe',
    'encrypt2',
    1,
    false,
    /SMB connect failed:.*Encryption required.*has no encryption/
  );
  await runProbe(
    port,
    'missing-share',
    'basic',
    1,
    false,
    /SMB connect failed:.*STATUS_BAD_NETWORK_NAME/
  );
  await runProbe(
    port,
    'probe',
    'basic',
    1,
    true,
    /SMB connect failed:.*STATUS_LOGON_FAILURE/
  );
  await runProbe(port, 'readonly', 'basic', 1, false, /SMB mkdir failed: -13/);
  for (const mode of ['cancel-read', 'cancel-write', 'disconnect-read'])
    await runTransferFault(port, mode);
  assert.equal(
    podman(['exec', name, 'find', '/share', '-mindepth', '1']).trim(),
    '/share/outside-link'
  );
  assert.equal(
    podman(['exec', name, 'cat', '/outside/sentinel.txt']),
    'outside sentinel\n'
  );
  console.log('PASS Samba probe and cleanup');
} finally {
  podman(['rm', '-f', name]);
}

for (const mode of ['cancel', 'timeout', 'disconnect']) {
  const listener = createServer();
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const address = listener.address();
  assert.ok(address && typeof address !== 'string');
  const process = spawn(
    resolve(root, '.build/smb-probe/smb-probe'),
    ['127.0.0.1', 'probe', 'elder', String(address.port), mode],
    {
      env,
      stdio: ['pipe', 'pipe', 'pipe'],
    }
  );
  const finished = once(process, 'exit');
  let output = '';
  process.stdout.on('data', (chunk) => {
    output += chunk;
  });
  process.stderr.on('data', (chunk) => {
    output += chunk;
  });
  const [socket] = await once(listener, 'connection');
  socket.on('error', (error: NodeJS.ErrnoException) => {
    assert.equal(error.code, 'ECONNRESET');
  });
  // The handshake is in flight before cancellation/disconnection is requested.
  await once(socket, 'data');
  if (mode === 'cancel') process.stdin.write('c');
  if (mode === 'disconnect') socket.destroy();
  const [status] = await finished;
  socket.destroy();
  listener.close();
  assert.equal(status, mode === 'disconnect' ? 1 : 0, output);
  if (mode !== 'disconnect') assert.match(output, /retired callback and FD/);
  console.log(`${mode}: ${output.trim()}`);
}
