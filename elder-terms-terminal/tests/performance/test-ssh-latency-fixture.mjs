// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { createConnection } from 'node:net';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';
import { setTimeout as delay } from 'node:timers/promises';
import { startSshLatencyFixture } from './ssh-latency-fixture.mts';

const exec = promisify(execFile);
const directory = await mkdtemp(join(tmpdir(), 'elder-ssh-cleanup-'));
let fixture;
let socket;

const members = async (group) => {
  const { stdout } = await exec('ps', ['-eo', 'pid=,pgid=,stat=']);
  return stdout
    .trim()
    .split('\n')
    .map((line) => line.trim().split(/\s+/))
    .filter(([, pgid, stat]) => Number(pgid) === group && !stat.startsWith('Z'))
    .map(([pid]) => Number(pid));
};

const waitFor = async (predicate, description) => {
  const deadline = Date.now() + 4000;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await delay(50);
  }
  throw new Error(`Timed out waiting for ${description}`);
};

try {
  fixture = await startSshLatencyFixture(directory, ['/bin/true']);
  socket = createConnection({ host: '127.0.0.1', port: fixture.port });
  socket.on('error', () => {});
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });
  await waitFor(
    async () =>
      (await members(fixture.serverPid)).some(
        (pid) => pid !== fixture.serverPid
      ),
    'a forked SSH server'
  );

  process.kill(fixture.serverPid, 'SIGTERM');
  await waitFor(
    async () => !(await members(fixture.serverPid)).includes(fixture.serverPid),
    'the listening parent to exit'
  );
  if (
    !(await members(fixture.serverPid)).some((pid) => pid !== fixture.serverPid)
  )
    throw new Error('The SSH child exited before cleanup was exercised');

  let timer;
  try {
    await Promise.race([
      fixture.stop(),
      new Promise((_, reject) => {
        timer = setTimeout(
          () => reject(new Error('Fixture stop did not finish')),
          3000
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
  await waitFor(
    async () => (await members(fixture.serverPid)).length === 0,
    'the process group to leave no live members'
  );
  await fixture.stop();
  console.log(
    'SSH fixture cleans orphaned children and tolerates repeated stop'
  );
} finally {
  socket?.destroy();
  if (fixture !== undefined) {
    try {
      process.kill(-fixture.serverPid, 'SIGKILL');
    } catch (error) {
      if (error.code !== 'ESRCH') throw error;
    }
  }
  await rm(directory, { recursive: true, force: true });
}
