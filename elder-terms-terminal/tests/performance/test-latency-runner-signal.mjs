// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { access, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';

const directory = await mkdtemp(join(tmpdir(), 'elder-latency-signal-'));
const output = join(directory, 'sample');
const script =
  process.argv[2] ??
  fileURLToPath(new URL('./run-latency.mts', import.meta.url));
const child = spawn(
  process.execPath,
  [
    script,
    output,
    '0',
    'plain',
    'idle',
    '1',
    '0',
    'cairo',
    'off',
    '30000',
    'on',
    'none',
    'ssh',
  ],
  { stdio: ['ignore', 'ignore', 'pipe'] }
);
let stderr = '';
child.stderr.on('data', (bytes) => {
  stderr += bytes.toString();
});
const exit = new Promise((resolve) =>
  child.once('close', (code, signal) => resolve({ code, signal }))
);

const waitFor = async (action, name) => {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    try {
      if (await action()) return;
    } catch {
      /* The artifact is being created. */
    }
    await delay(50);
  }
  throw new Error(`Timed out waiting for ${name}: ${stderr}`);
};

try {
  await waitFor(async () => {
    await access(join(output, 'terminal.log'));
    return true;
  }, 'terminal launch');
  await delay(200);
  if (child.exitCode !== null || child.signalCode !== null)
    throw new Error(`Runner exited before interruption: ${stderr}`);
  child.kill('SIGTERM');
  let timer;
  let result;
  try {
    result = await Promise.race([
      exit,
      new Promise((_, reject) => {
        timer = setTimeout(
          () => reject(new Error('Runner ignored SIGTERM')),
          5000
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
  if (result.signal !== null)
    throw new Error(
      `Runner was terminated by signal without cleanup: ${result.signal}`
    );
  await access(join(output, 'ssh-fixture', 'server.log'));
  console.log('Runner SIGTERM ran SSH fixture cleanup');
} finally {
  if (child.exitCode === null && child.signalCode === null)
    child.kill('SIGKILL');
  await rm(directory, { recursive: true, force: true });
}
