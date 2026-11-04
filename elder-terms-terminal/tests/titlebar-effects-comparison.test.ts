// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { createTestEvidence } from '../../shared/tests/connection-test-helpers.ts';

const exec = promisify(execFile);

it('records and compares weak, medium and strong titlebar motion, BEL merging, cycle continuity and return to each background', async (context) => {
  const evidence = createTestEvidence(context);
  const directory = join(evidence.directory, 'comparison');
  await exec(
    'xvfb-run',
    [
      '-a',
      resolve('../.build/elder-terms-terminal/titlebar-effects-demo'),
      directory,
    ],
    {
      cwd: resolve('..'),
      env: {
        ...process.env,
        G_DEBUG: 'fatal-criticals',
        NO_AT_BRIDGE: '1',
        GDK_SCALE: '1',
      },
    }
  );
  const frames = (await readFile(join(directory, 'frames.csv'), 'utf8'))
    .trim()
    .split('\n')
    .slice(1)
    .map((line) => line.split(',').map(Number));
  const rows = (await readFile(join(directory, 'rows.csv'), 'utf8'))
    .trim()
    .split('\n')
    .map((line) => line.split(',').map(Number));
  expect(frames.length).toBeGreaterThan(30);
  // Preserve observed frame timestamps in the video, including slow CI frames.
  let manifest = 'ffconcat version 1.0\n';
  for (let i = 0; i < frames.length; ++i) {
    manifest += `file frame-${String(frames[i][0]).padStart(4, '0')}.png\n`;
    if (i + 1 < frames.length)
      manifest += `duration ${(frames[i + 1][1] - frames[i][1]) / 1e6}\n`;
    if (i) {
      expect(frames[i][1]).toBeGreaterThan(frames[i - 1][1]);
      expect(frames[i][2]).toBeGreaterThan(frames[i - 1][2]);
    }
  }
  await writeFile(join(directory, 'timing.ffconcat'), manifest);
  await exec('ffmpeg', [
    '-y',
    '-loglevel',
    'error',
    '-f',
    'concat',
    '-safe',
    '0',
    '-i',
    join(directory, 'timing.ffconcat'),
    '-fps_mode',
    'vfr',
    '-c:v',
    'ffv1',
    '-threads',
    '1',
    join(directory, 'comparison.mkv'),
  ]);
  const at = async (time: number): Promise<PNG> => {
    const frame = frames.reduce((best, next) =>
      Math.abs(next[1] - time) < Math.abs(best[1] - time) ? next : best
    );
    return PNG.sync.read(
      await readFile(
        join(directory, `frame-${String(frame[0]).padStart(4, '0')}.png`)
      )
    );
  };
  const baseline = await at(0);
  const start = frames.find((frame) => frame[3] >= 1)![1];
  const early = await at(start + 300000);
  const later = await at(start + 1000000);
  const boundaryBefore = await at(start + 1170000);
  const boundaryAfter = await at(start + 1230000);
  const ended = await at(4100000);
  const bellStart = frames.find((frame) => frame[3] >= 4)![1];
  const bell = await at(bellStart);
  const repeated = await at(bellStart + 130000);
  const returned = await at(bellStart + 300000);
  const differences = (png: PNG, row: number): number[] => {
    const [left, top, width] = rows[row];
    // Exclude native buttons, which deliberately retain their own backgrounds.
    return Array.from({ length: 61 }, (_, index) => {
      const x = Math.round(left + width * (0.05 + index / 100));
      const offset = ((top + 3) * png.width + x) * 4;
      return Math.abs(png.data[offset] - baseline.data[offset]);
    });
  };
  for (let row = 0; row < 9; ++row) {
    const first = differences(early, row);
    const second = differences(later, row);
    expect(first.indexOf(Math.max(...first))).toBeLessThan(
      second.indexOf(Math.max(...second))
    );
    expect(Math.max(...differences(boundaryBefore, row))).toBeGreaterThan(4);
    expect(Math.max(...differences(boundaryAfter, row))).toBeGreaterThan(4);
    expect(Math.max(...differences(ended, row))).toBeLessThanOrEqual(1);
    // The minimum across the header excludes the moving band: it isolates BEL.
    expect(Math.min(...differences(bell, row))).toBeGreaterThan(15);
    expect(Math.min(...differences(repeated, row))).toBeLessThan(
      Math.min(...differences(bell, row))
    );
    expect(Math.min(...differences(returned, row))).toBeLessThanOrEqual(1);
  }
  for (const first of [0, 3, 6]) {
    for (const [offset, multiplier] of [
      [1, 1.5],
      [2, 2],
    ]) {
      const gradientRatio =
        Math.max(...differences(early, first + offset)) /
        Math.max(...differences(early, first));
      const bellRatio =
        Math.min(...differences(bell, first + offset)) /
        Math.min(...differences(bell, first));
      expect(Math.abs(gradientRatio - multiplier)).toBeLessThan(0.15);
      expect(Math.abs(bellRatio - multiplier)).toBeLessThan(0.15);
    }
  }
  await evidence.log('Comparison complete', {
    directory,
    frames: frames.length,
    start,
    bellStart,
  });
});
