import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const name = `elder-smb1-fault-${process.pid}`;
const image =
  process.env.ELDER_SMB1_TEST_IMAGE ?? 'localhost/elder-plan18-samba';
const env = {
  ...process.env,
  ELDER_SMB_PASSWORD: randomBytes(24).toString('hex'),
};
const podman = (args: string[]): string =>
  execFileSync('podman', args, { env, encoding: 'utf8' });
try {
  podman(['image', 'exists', image]);
} catch {
  const architecture = process.arch === 'x64' ? 'amd64' : process.arch;
  execFileSync(
    'podman',
    [
      'build',
      '--platform',
      `linux/${architecture}`,
      '-t',
      image,
      '-f',
      resolve(directory, 'Containerfile.smb1'),
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
    '--env',
    'ELDER_SMB_PASSWORD',
    '--env',
    'ELDER_SMB_PORT=1445',
    '-v',
    `${root}:${root}:ro`,
    '-v',
    `${process.execPath}:/elder-node:ro`,
    image,
    'sh',
    resolve(directory, 'samba1-entry.sh'),
  ],
  { env, stdio: ['ignore', 'pipe', 'pipe'] }
);
try {
  await new Promise<void>((ready, reject) => {
    let output = '';
    const timeout = setTimeout(
      () => reject(new Error(`Samba startup timeout: ${output}`)),
      30000
    );
    const collect = (data: Buffer) => {
      output += data;
      if (output.includes('waiting for connections')) {
        clearTimeout(timeout);
        ready();
      }
    };
    server.stdout.on('data', collect);
    server.stderr.on('data', collect);
    server.once('error', reject);
    server.once('exit', (code) => {
      clearTimeout(timeout);
      reject(new Error(`Samba exited ${code}: ${output}`));
    });
  });
  const namespacePid = podman([
    'inspect',
    '--format',
    '{{.State.Pid}}',
    name,
  ]).trim();
  for (const mode of [
    'stat-framing',
    'cancel',
    'timeout',
    'disconnect',
    'worker-exit',
    'cancel-create',
    'disconnect-create',
    'cancel-write',
    'cancel-commit',
    'short-write',
    'zero-write',
    'invalid-name',
    'connect-timeout',
    'partial-shares',
  ]) {
    const relay = spawn(
      'podman',
      ['exec', '-i', name, '/elder-node', resolve(directory, 'smb1-relay.mts')],
      { env, stdio: ['pipe', 'pipe', 'pipe'] }
    );
    let relayOutput = '';
    await new Promise<void>((ready, reject) => {
      const timer = setTimeout(
        () => reject(new Error(`Relay startup timeout: ${relayOutput}`)),
        30000
      );
      relay.stdout.on('data', (data: Buffer) => {
        relayOutput += data;
        if (relayOutput.includes('READY relay')) {
          clearTimeout(timer);
          ready();
        }
      });
      relay.stderr.on('data', (data: Buffer) => {
        relayOutput += data;
      });
      relay.once('error', reject);
      relay.once('exit', (code) => {
        clearTimeout(timer);
        reject(new Error(`Relay exited ${code}: ${relayOutput}`));
      });
    });
    const driver = spawn(
      'podman',
      [
        'unshare',
        'nsenter',
        '-t',
        namespacePid,
        '-n',
        resolve(process.argv[2]),
        mode,
      ],
      { env: { ...env, LC_ALL: 'C' }, stdio: ['pipe', 'pipe', 'pipe'] }
    );
    let output = '',
      armed = false,
      injected = false;
    driver.stdout.on('data', (data: Buffer) => {
      output += data;
      if (!armed && output.includes('READY fault')) {
        armed = true;
        relay.stdin.write(mode);
      }
    });
    driver.stderr.on('data', (data: Buffer) => {
      output += data;
    });
    relay.stdout.on('data', (data: Buffer) => {
      const text = data.toString();
      if (text.includes('ARMED')) driver.stdin.write('r');
      if (text.includes('HELD')) {
        injected = true;
        if (mode.startsWith('cancel') || mode === 'worker-exit')
          driver.stdin.write('c');
      }
    });
    const watchdog = setTimeout(() => driver.kill('SIGKILL'), 30000);
    try {
      const [status] = await once(driver, 'exit');
      assert.equal(status, 0, `${output}\n${relayOutput}`);
      assert.ok(
        injected,
        'The relay must inject the requested condition into a real server operation'
      );
      assert.match(output, /PASS SMB1 fault/);
      console.log(`${mode}: ${output.trim()}`);
    } finally {
      clearTimeout(watchdog);
      relay.stdin.end();
      await once(relay, 'exit');
    }
  }
} finally {
  podman(['rm', '-f', name]);
}
