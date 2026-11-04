// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile, writeFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest, readWindowCellLayout } from './gtk-test-helpers.ts';
import {
  withTemporaryDirectory,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

for (const [sound, color, scale, compact] of [
  ['default', '#202020', '1', 'false'],
  ['custom', '#245b76', '2', 'true'],
  ['removed', '#eeeeee', '1', 'true'],
]) {
  it(`records a bounded BEL flash with ${sound} audio and preserves terminal geometry`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const peer = join(directory, 'peer.mjs');
      const config = join(directory, 'connection.ini');
      const wav = join(directory, 'bell.wav');
      // A valid short PCM file lets the existing player select the custom path.
      const audio = Buffer.alloc(2044);
      audio.write('RIFF');
      audio.writeUInt32LE(2036, 4);
      audio.write('WAVEfmt ', 8);
      audio.writeUInt32LE(16, 16);
      audio.writeUInt16LE(1, 20);
      audio.writeUInt16LE(1, 22);
      audio.writeUInt32LE(8000, 24);
      audio.writeUInt32LE(16000, 28);
      audio.writeUInt16LE(2, 32);
      audio.writeUInt16LE(16, 34);
      audio.write('data', 36);
      audio.writeUInt32LE(2000, 40);
      await writeFile(wav, audio);
      await writeFile(
        peer,
        `import { writeFileSync } from 'node:fs';
process.stdin.setRawMode(true); process.stdin.resume(); writeFileSync(${JSON.stringify(join(directory, 'ready'))}, 'READY');
process.stdout.write('READY'); process.stdin.on('data', () => process.stdout.write('\\x07\\x07\\x07'));`
      );
      await writeFile(
        config,
        `[general]\nexterior_background=${color}\ncompact_mode=${compact}\n[terminal]\naccessibility_enabled=true\nbell_sound=${sound === 'default' ? 'default' : wav}\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          await waitForResult(async () =>
            expect(await readFile(join(directory, 'ready'), 'utf8')).toBe(
              'READY'
            )
          );
          const layout = await readWindowCellLayout(app);
          if (sound === 'removed') await rm(wav);
          const header = await app.getById('header_bar');
          const before = await header.capture();
          const png = PNG.sync.read(before.image);
          // AT-SPI header bounds are logical coordinates. X11 recording uses
          // physical pixels; scale the capture region and sample positions.
          const factor = Number(scale);
          const width = png.width * factor;
          const height = png.height * factor;
          const env = await app.environment();
          const recorder = spawn(
            'ffmpeg',
            [
              '-y',
              '-loglevel',
              'error',
              '-f',
              'x11grab',
              '-framerate',
              '60',
              '-draw_mouse',
              '0',
              '-video_size',
              `${width}x${height}`,
              '-i',
              `${env.DISPLAY}+${before.bounds.x * factor},${before.bounds.y * factor}`,
              '-t',
              '2',
              '-c:v',
              'ffv1',
              '-threads',
              '1',
              join(evidence.directory, 'bell.mkv'),
              '-t',
              '2',
              '-f',
              'rawvideo',
              '-pix_fmt',
              'rgb24',
              '-threads',
              '1',
              'pipe:1',
            ],
            { env }
          );
          const finished = once(recorder, 'close');
          let pending = Buffer.alloc(0);
          const samples: number[] = [];
          let stderr = '';
          recorder.stderr.on('data', (chunk: Buffer) => {
            stderr += chunk.toString();
          });
          recorder.stdout.on('data', (chunk: Buffer) => {
            pending = Buffer.concat([pending, chunk]);
            const size = width * height * 3;
            while (pending.length >= size) {
              const offset = (3 * factor * width + Math.round(width * 0.3)) * 3;
              samples.push(pending[offset]);
              pending = pending.subarray(size);
            }
          });
          let inputFrame = 0;
          try {
            await waitForResult(async () =>
              expect(samples.length).toBeGreaterThan(2)
            );
            inputFrame = samples.length;
            await app.input.pressKey('b');
            const [status] = await finished;
            expect(status, stderr).toBe(0);
          } finally {
            if (recorder.exitCode === null && recorder.signalCode === null) {
              recorder.kill('SIGTERM');
              await finished;
            }
          }
          await evidence.log('BEL video samples', { samples, inputFrame });
          const output = await app.output();
          if (sound === 'removed')
            expect(output.stderr).toContain(
              'Failed to play terminal bell sound'
            );
          else
            expect(output.stderr).not.toContain(
              'Failed to play terminal bell sound'
            );
          const baseline = samples[inputFrame - 1];
          expect(
            Math.max(
              ...samples
                .slice(inputFrame)
                .map((value) => Math.abs(value - baseline))
            )
          ).toBeGreaterThan(20);
          expect(samples.slice(-5)).toEqual(Array(5).fill(baseline));
          const active = samples
            .slice(inputFrame)
            .filter((value) => Math.abs(value - baseline) > 2);
          expect(active.length).toBeGreaterThan(2);
          expect(active.length).toBeLessThan(22);
          const after = await readWindowCellLayout(app);
          expect([after.columns, after.rows]).toEqual([
            layout.columns,
            layout.rows,
          ]);
          expect((await header.capture()).bounds.height).toBe(
            before.bounds.height
          );
          await promisify(execFile)('ffmpeg', [
            '-y',
            '-loglevel',
            'error',
            '-sseof',
            '-0.1',
            '-i',
            join(evidence.directory, 'bell.mkv'),
            '-frames:v',
            '1',
            join(evidence.directory, 'header-after-bell.png'),
          ]);
        },
        { env: { GDK_SCALE: scale } }
      );
    });
  });
}

it('records OSC title motion and simultaneous BEL while every final title is immediately visible', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const peer = join(directory, 'titles.mjs');
    const config = join(directory, 'connection.ini');
    await writeFile(
      peer,
      `import { writeFileSync } from 'node:fs';
process.stdin.setRawMode(true); process.stdin.resume(); writeFileSync(${JSON.stringify(join(directory, 'ready'))}, 'READY');
process.stdin.on('data', (data) => { for (const key of data.toString()) {
  const title = key === 'a' ? 'effect-alpha' : 'effect-beta';
  process.stdout.write((key === 'b' ? '\\x07' : '') + '\\x1b]0;' + title + '\\x1b\\\\');
}});`
    );
    await writeFile(
      config,
      `[general]\nexterior_background=#202020\n[local]\ncommand_line=${process.execPath} ${peer}\n`
    );
    await runGtkTest(context, ['-c', config], async (app, evidence) => {
      await waitForResult(async () =>
        expect(await readFile(join(directory, 'ready'), 'utf8')).toBe('READY')
      );
      const header = await app.getById('header_bar');
      const label = expectElementKind(
        await app.getById('header_title_label'),
        'label'
      );
      const capture = await header.capture();
      const png = PNG.sync.read(capture.image);
      const env = await app.environment();
      const recorder = spawn(
        'ffmpeg',
        [
          '-y',
          '-loglevel',
          'error',
          '-f',
          'x11grab',
          '-framerate',
          '60',
          '-draw_mouse',
          '0',
          '-video_size',
          `${png.width}x${png.height}`,
          '-i',
          `${env.DISPLAY}+${capture.bounds.x},${capture.bounds.y}`,
          '-t',
          '4',
          '-c:v',
          'ffv1',
          '-threads',
          '1',
          join(evidence.directory, 'titles.mkv'),
          '-t',
          '4',
          '-f',
          'rawvideo',
          '-pix_fmt',
          'rgb24',
          '-threads',
          '1',
          'pipe:1',
        ],
        { env }
      );
      const finished = once(recorder, 'close');
      let pending = Buffer.alloc(0);
      const samples: number[][] = [];
      const updates: { title: string; frame: number }[] = [];
      let stderr = '';
      recorder.stderr.on('data', (chunk: Buffer) => {
        stderr += chunk.toString();
      });
      recorder.stdout.on('data', (chunk: Buffer) => {
        pending = Buffer.concat([pending, chunk]);
        const size = png.width * png.height * 3;
        while (pending.length >= size) {
          samples.push(
            [0.2, 0.4, 0.6].map(
              (x) => pending[(3 * png.width + Math.round(png.width * x)) * 3]
            )
          );
          pending = pending.subarray(size);
        }
      });
      try {
        await waitForResult(async () =>
          expect(samples.length).toBeGreaterThan(2)
        );
        for (const [key, title] of [
          ['a', 'effect-alpha'],
          ['b', 'effect-beta'],
          ['a', 'effect-alpha'],
        ]) {
          await app.input.pressKey(key);
          await waitForResult(async () =>
            expect(await label.text()).toContain(title)
          );
          updates.push({ title, frame: samples.length });
        }
        const [status] = await finished;
        expect(status, stderr).toBe(0);
      } finally {
        if (recorder.exitCode === null && recorder.signalCode === null) {
          recorder.kill('SIGTERM');
          await finished;
        }
      }
      await evidence.log('Title video samples and observed updates', {
        samples,
        updates,
      });
      expect(
        samples.some((frame) => Math.max(...frame) - Math.min(...frame) > 8)
      ).toBe(true);
      expect(samples.some((frame) => frame[0] > frame[1] + 5)).toBe(true);
      expect(samples.some((frame) => frame[2] > frame[1] + 5)).toBe(true);
      expect(samples.some((frame) => Math.min(...frame) > 50)).toBe(true);
      expect(samples.slice(-5)).toEqual(
        Array.from({ length: 5 }, () => [32, 32, 32])
      );
      expect(await label.text()).toContain('effect-alpha');
      await evidence.captureEvidence('title-settled', () => header.capture());
    });
  });
});
