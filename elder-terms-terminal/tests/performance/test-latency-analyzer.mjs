// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const run = promisify(execFile);
const analyzer = fileURLToPath(
  new URL('./analyze-latency.mts', import.meta.url)
);
const directory = await mkdtemp(join(tmpdir(), 'elder-latency-analysis-'));
const trace =
  [
    'kind\tstart_ns\tend_ns\tcpu_ns\tsequence\tvalue\thistory\trevision',
    'gtk_key\t150\t160\t10\t0\t1\t0\t0',
    'received\t250\t260\t10\t0\t1\t0\t0',
    'parsed\t300\t310\t10\t0\t1\t0\t1',
    'draw\t400\t500\t100\t0\t1\t0\t1',
    'composite\t600\t900\t300\t0\t1\t0\t1',
  ].join('\n') + '\n';
const input = async (visible) => {
  await writeFile(
    join(directory, 'events.tsv'),
    `key\tsent_ns\tvisible_ns\n0\t100\t${visible}\n`
  );
  await writeFile(
    join(directory, 'peer.tsv'),
    'key\treceived_ns\twritten_ns\toutput_bytes\toutput_lines\toutput_updates\n0\t200\t300\t1\t1\t1\n'
  );
  await writeFile(join(directory, 'trace.tsv'), trace);
  await writeFile(
    join(directory, 'configuration.json'),
    JSON.stringify({ count: 1, tracing: 'light' })
  );
};
try {
  await input(800);
  await run(process.execPath, [analyzer, directory]);
  const summary = JSON.parse(
    await readFile(join(directory, 'summary.json'), 'utf8')
  );
  if (
    summary.count !== 1 ||
    summary.samples[0].totalMs !== 0.0007 ||
    summary.samples[0].toCompositeMs !== 0.0008
  )
    throw new Error('Visible pixels during composite were not recorded');
  await input(550);
  try {
    await run(process.execPath, [analyzer, directory]);
    throw new Error('Pixels preceding composite start were accepted');
  } catch (error) {
    if (!String(error.stderr ?? '').includes('Invalid phase order'))
      throw error;
  }
  console.log('Composite/pixel overlap and invalid ordering passed');
} finally {
  await rm(directory, { recursive: true, force: true });
}
