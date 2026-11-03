import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const name = `elder-smb1-library-${process.pid}`;
const image = 'localhost/elder-plan18-samba';
const env = { ...process.env, ELDER_SMB_PASSWORD: randomBytes(24).toString('hex') };
const podman = (args: string[]): string => execFileSync('podman', args, { env, encoding: 'utf8' });
try { podman(['image', 'exists', image]); }
catch {
  execFileSync('podman', ['build', '-t', image, '-f', resolve(directory, 'Containerfile.smb1'), directory], { stdio: 'inherit' });
}
// Keep port 445 inside the disposable container's network namespace. The host
// needs neither privileged ports nor a change to ip_unprivileged_port_start.
const server = spawn('podman', ['run', '--rm', '--name', name,
  '--env', 'ELDER_SMB_PASSWORD', '-v', `${root}:${root}:ro`,
  image, 'sh', resolve(directory, 'samba1-entry.sh')], { env, stdio: ['ignore', 'pipe', 'pipe'] });
try {
  await new Promise<void>((ready, reject) => {
    let output = '';
    const timeout = setTimeout(() => reject(new Error(`Samba startup timeout: ${output}`)), 30000);
    const collect = (data: Buffer) => {
      output += data;
      if (output.includes('waiting for connections')) { clearTimeout(timeout); ready(); }
    };
    server.stdout.on('data', collect);
    server.stderr.on('data', collect);
    server.once('error', reject);
    server.once('exit', (code) => { clearTimeout(timeout); reject(new Error(`Samba exited ${code}: ${output}`)); });
  });
  const namespacePid = podman(['inspect', '--format', '{{.State.Pid}}', name]).trim();
  let failures = 0;
  const cases = process.argv[3] === '--client' ? [['TCP', 'client']] : [['TCP', 'files'], ['NBT', 'files'], ['TCP', 'names']];
  for (const [transport, mode] of cases) {
    const driver = spawn('podman', ['unshare', 'nsenter', '-t', namespacePid, '-n',
      resolve(process.argv[2]), '127.0.0.1', transport, ...(mode === 'names' ? ['names'] : [])], { env: { ...env, LC_ALL: 'C' }, stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    driver.stdout.on('data', (chunk: Buffer) => { output += chunk; });
    driver.stderr.on('data', (chunk: Buffer) => { output += chunk; });
    const watchdog = setTimeout(() => driver.kill('SIGKILL'), 30000);
    try {
      const [status] = await once(driver, 'exit');
      console.log(`${transport}: ${output.trim()}`);
      if (status !== 0) ++failures;
      else assert.match(output, /PASS SMB1 (client|public file operations|complete directory listing)/);
    } finally { clearTimeout(watchdog); }
  }
  assert.equal(failures, 0, `${failures} SMB1 capability probes failed`);
} finally { podman(['rm', '-f', name]); }
