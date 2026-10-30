// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const exec = promisify(execFile);
const header =
  'kind\tstart_ns\tend_ns\tcpu_ns\tsequence\tvalue\thistory\trevision\tstatus\tpending\tdepth\n';
const observations = [
  ['gtk_key', 110, 111, 1, 1, 107, 0, 0, 0, 0, 0],
  ['received', 125, 130, 1, 1, 40, 0, 0, 0, 0, 0],
  ['parsed', 135, 140, 1, 1, 40, 1000, 9, 0, 0, 0],
  ['frame', 145, 150, 3, 1, 1, 1000, 9, 0, 0, 0],
  ['draw', 155, 160, 3, 1, 77, 1000, 9, 0, 0, 0],
  ['composite', 161, 162, 1, 1, 88, 0, 0, 0, 0, 0],
  ['composite', 165, 170, 2, 1, 77, 0, 0, 0, 0, 0],
];

it.each(['valid', 'empty', 'wrong-window', 'reversed', 'after-visible'])(
  'accepts only corresponding ordered input and presentation observations: %s',
  async (scenario) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-latency-analysis-'));
    try {
      let rows = observations.map((row) => [...row]);
      if (scenario === 'empty') rows = [];
      if (scenario === 'wrong-window')
        rows = rows.filter((row) => !(row[0] === 'composite' && row[5] === 77));
      if (scenario === 'reversed') rows[2][1] = rows[2][2] = 115;
      if (scenario === 'after-visible') rows[6][1] = rows[6][2] = 210;
      await writeFile(
        join(directory, 'configuration.json'),
        JSON.stringify({ tracing: 'detail', count: 1 })
      );
      await writeFile(
        join(directory, 'events.tsv'),
        'key\tsent_ns\tvisible_ns\n1\t100\t200\n'
      );
      await writeFile(
        join(directory, 'peer.tsv'),
        'key\treceived_ns\twritten_ns\toutput_bytes\toutput_lines\toutput_updates\n1\t120\t125\t40\t1\t0\n'
      );
      await writeFile(
        join(directory, 'trace.tsv'),
        header + rows.map((row) => row.join('\t')).join('\n')
      );
      const analyze = async () =>
        await exec(process.execPath, [
          'tests/performance/analyze-latency.mts',
          directory,
        ]);
      if (scenario === 'valid') {
        await analyze();
        const result = JSON.parse(
          await readFile(join(directory, 'summary.json'), 'utf8')
        );
        expect(result.samples[0].toCompositeMs).toBe(0.00007);
      } else {
        await expect(analyze()).rejects.toThrow();
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
