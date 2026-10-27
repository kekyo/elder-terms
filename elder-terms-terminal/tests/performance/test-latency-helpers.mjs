// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const directory = await mkdtemp(join(tmpdir(), 'elder-latency-helper-'));
const peer = process.argv[2];
if (!peer) throw new Error('Expected latency peer binary path');
const waitForText = (state, text) =>
  new Promise((resolve, reject) => {
    if (state.output.includes(text)) return resolve();
    const timeout = setTimeout(
      () => reject(new Error(`Missing ${text}`)),
      30000
    );
    state.waiters.push(() => {
      if (!state.output.includes(text)) return;
      clearTimeout(timeout);
      resolve();
    });
  });

try {
  for (const [history, mode, load] of [
    [0, 'plain', 'idle'],
    [1000, 'url', 'append'],
    [1001, 'wrapped', 'overwrite'],
  ]) {
    const report = join(directory, `${history}-${mode}-${load}.tsv`);
    const command = `${peer} ${report} ${history} ${mode} ${load} 10 4`;
    const child = spawn(
      'script',
      ['-q', '-f', '-e', '-c', command, '/dev/null'],
      {
        stdio: ['pipe', 'pipe', 'pipe'],
      }
    );
    const state = { output: '', waiters: [] };
    child.stdout.on('data', (bytes) => {
      state.output += bytes.toString('latin1');
      for (const notify of state.waiters) notify();
    });
    await waitForText(state, 'elder-latency:ready');
    child.stdin.write('g');
    if (load === 'append') await waitForText(state, 'elder-latency 00001023');
    if (load === 'overwrite')
      await waitForText(state, 'elder-latency wrapped update');
    child.stdin.write('k');
    await waitForText(state, 'elder-latency:1');
    child.stdin.write('skq');
    await waitForText(state, 'elder-latency:2');
    child.stdin.end();
    const exit = await new Promise((resolve) => child.on('close', resolve));
    if (exit !== 0) throw new Error(`Peer exited ${exit}: ${state.output}`);
    const lines = (await readFile(report, 'utf8')).trim().split('\n');
    if (
      lines.length !== 3 ||
      !lines[0].startsWith('key\treceived_ns\twritten_ns\t')
    )
      throw new Error(`Unexpected report: ${lines.join(' | ')}`);
    for (let index = 1; index < 3; index++) {
      const cells = lines[index].split('\t').map(Number);
      if (cells[0] !== index || cells[1] <= 0 || cells[2] < cells[1])
        throw new Error(`Bad key record: ${lines[index]}`);
    }
    if (
      !state.output.includes('elder-latency:ready') ||
      !state.output.includes('elder-latency:1') ||
      !state.output.includes('elder-latency:2')
    )
      throw new Error('Missing visible sequence');
  }
  console.log('Latency peer protocol passed');
} finally {
  await rm(directory, { recursive: true, force: true });
}
