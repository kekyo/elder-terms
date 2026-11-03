import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const name = `elder-smb1-window-${process.pid}`;
const password = randomBytes(24).toString('hex');
const env = { ...process.env, ELDER_SMB_PASSWORD: password };
const podman = (args: string[]): string =>
  execFileSync('podman', args, { env, encoding: 'utf8' });
try {
  podman(['image', 'exists', 'localhost/elder-plan18-samba']);
} catch {
  podman([
    'build',
    '-t',
    'localhost/elder-plan18-samba',
    '-f',
    resolve(directory, 'Containerfile.smb1'),
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
    '--env',
    'ELDER_SMB_PASSWORD',
    '-v',
    `${root}:${root}:ro`,
    'localhost/elder-plan18-samba',
    'sh',
    resolve(directory, 'samba1-entry.sh'),
  ],
  { env, stdio: ['ignore', 'pipe', 'pipe'] }
);
try {
  await new Promise<void>((ready, reject) => {
    let output = '';
    const timer = setTimeout(
      () => reject(new Error(`Samba startup timeout: ${output}`)),
      30000
    );
    const collect = (data: Buffer) => {
      output += data;
      if (output.includes('waiting for connections')) {
        clearTimeout(timer);
        ready();
      }
    };
    server.stdout.on('data', collect);
    server.stderr.on('data', collect);
    server.once('error', reject);
    server.once('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`Samba exited ${code}: ${output}`));
    });
  });
  podman([
    'exec',
    name,
    'sh',
    '-c',
    "printf 'single SMB file\\n' >/share/single.txt; mkdir /share/folder; printf 'nested SMB file\\n' >/share/folder/nested.txt",
  ]);
  const pid = podman(['inspect', '--format', '{{.State.Pid}}', name]).trim();
  // Run the app, D-Bus and accessibility services in the same user/network
  // namespaces, so credentials and abstract UNIX socket names agree.
  const child = spawn(
    'podman',
    [
      'unshare',
      'nsenter',
      '-t',
      pid,
      '-n',
      process.execPath,
      resolve(root, 'node_modules/vitest/vitest.mjs'),
      'run',
      'tests/smb-window.test.ts',
      '-t',
      'opens SMB 1',
    ],
    {
      cwd: resolve(directory, '../..'),
      stdio: 'inherit',
      env: {
        ...env,
        ELDER_SMB1_NAMESPACE: '1',
        ELDER_SMB_UI_ADDRESS: '127.0.0.1',
        ELDER_SMB_UI_PORT: '445',
        ELDER_SMB_UI_USER: 'elder',
        ELDER_SMB_UI_SHARE: 'probe',
        ELDER_SMB_UI_EMPTY_SHARE: 'empty',
        ELDER_SMB_UI_DENIED_SHARE: 'denied',
        ELDER_SMB_UI_PASSWORD: password,
      },
    }
  );
  const [status] = await once(child, 'exit');
  assert.equal(status, 0, 'SMB1 window test failed');
  assert.equal(
    podman(['exec', name, 'cat', '/share/送信😀/日本語/内容.txt']),
    'recursive SMB1 upload\n'
  );
  assert.equal(
    podman(['exec', name, 'cat', '/share/kept.txt']),
    'replacement upload\n'
  );
} finally {
  podman(['rm', '-f', name]);
}
