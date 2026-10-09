import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';

/** Runs a gateway that resolves proxy-test.invalid to the test service on loopback. */
export const withSshGateway = async (executable, directory, check) => {
  const key = join(directory, 'gateway-' + randomUUID());
  const generating = spawn('ssh-keygen', [
    '-q',
    '-t',
    'ed25519',
    '-N',
    '',
    '-f',
    key,
  ]);
  const [keyCode] = await once(generating, 'close');
  if (keyCode !== 0)
    throw new Error('Could not generate SSH gateway fixture key');
  const child = spawn(executable, [key], {
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  const completion = once(child, 'close');
  const lines = createInterface({ input: child.stdout });
  const requests = [];
  const ready = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', (code) =>
      reject(new Error('SSH gateway exited before readiness: ' + code))
    );
    lines.on('line', (line) => {
      if (line.startsWith('PORT ')) resolve(Number(line.slice(5)));
      else if (line.startsWith('FORWARD ')) {
        const [, host, port] = line.split(' ');
        requests.push({ host, port: Number(port) });
      }
    });
  });
  try {
    const port = await ready;
    await check({ port, requests, knownHosts: key + '-known-hosts' });
    const [code] = await completion;
    if (code !== 0) throw new Error('SSH gateway validation failed: ' + code);
    if (!requests.length)
      throw new Error('No requests passed through the SSH gateway');
  } finally {
    if (child.exitCode === null && child.signalCode === null) child.kill();
    await completion;
    lines.close();
  }
};
