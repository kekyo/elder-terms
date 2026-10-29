import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { once } from 'node:events';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const name = `elder-smb-client-${process.pid}`;
const env = {
  ...process.env,
  ELDER_SMB_PASSWORD: randomBytes(24).toString('hex'),
};
const podman = (args: string[]): string =>
  execFileSync('podman', args, { env, encoding: 'utf8' });
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
  assert.equal(
    podman(['exec', name, 'cat', '/outside/sentinel.txt']),
    'outside sentinel\n'
  );
} finally {
  podman(['rm', '-f', name]);
}
