// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = await mkdtemp(join(tmpdir(), 'elder-latency-runner-'));
const output = join(directory, 'sample');
const script = fileURLToPath(new URL('./run-latency.mts', import.meta.url));
const capture = process.argv[2] ?? 'none';
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
    '0',
    'on',
    capture,
  ],
  {
    env: {
      ...process.env,
      ELDER_LATENCY_COLUMNS: '88',
      ELDER_LATENCY_ROWS: '31',
      ELDER_LATENCY_FONT: '15',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  }
);
let stderr = '';
child.stderr.on('data', (bytes) => {
  stderr += bytes.toString();
});
try {
  const status = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', resolve);
  });
  if (status !== 0) throw new Error(`Runner exited ${status}: ${stderr}`);
  const config = await readFile(join(output, 'connection.ini'), 'utf8');
  if (!config.includes('width=88\nheight=31\nfont_size_dip=15\n'))
    throw new Error('Runner did not apply the requested display dimensions');
  const metadata = JSON.parse(
    await readFile(join(output, 'configuration.json'), 'utf8')
  );
  if (
    metadata.columns !== 88 ||
    metadata.rows !== 31 ||
    metadata.font !== 15 ||
    metadata.capture !== capture
  )
    throw new Error('Runner did not record the measurement settings');
  const events = (await readFile(join(output, 'events.tsv'), 'utf8'))
    .trim()
    .split('\n');
  if (events.length !== 2) throw new Error('The key was not measured');
  await access(join(output, 'frames.tsv'));
  await access(join(output, 'terminal.log'));
  if (capture === 'none') {
    for (const filename of ['video.ffconcat', 'video.mkv', 'frame-00000.ppm']) {
      try {
        await access(join(output, filename));
        throw new Error(`Capture none unexpectedly created ${filename}`);
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
      }
    }
  } else {
    await access(join(output, 'video.ffconcat'));
    await access(join(output, 'video.mkv'));
    await access(join(output, 'frame-00000.ppm'));
  }
  console.log(`Latency runner capture ${capture} and display settings passed`);
} finally {
  if (child.exitCode === null && child.signalCode === null)
    child.kill('SIGTERM');
  await rm(directory, { recursive: true, force: true });
}
