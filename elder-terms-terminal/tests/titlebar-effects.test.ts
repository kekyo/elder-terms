// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile, writeFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest, readTerminalGridLayout } from './gtk-test-helpers.ts';
import { withTemporaryDirectory } from '../../shared/tests/connection-test-helpers.ts';

for (const sound of ['default', 'custom', 'removed']) {
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
        `[general]\nexterior_background=#202020\n[terminal]\naccessibility_enabled=true\nbell_sound=${sound === 'default' ? 'default' : wav}\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await runGtkTest(context, ['-c', config], async (app, evidence) => {
        await waitForResult(async () =>
          expect(await readFile(join(directory, 'ready'), 'utf8')).toBe('READY')
        );
        const layout = await readTerminalGridLayout(app);
        if (sound === 'removed') await rm(wav);
        const header = await app.getById('header_bar');
        const before = await header.capture();
        const png = PNG.sync.read(before.image);
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
            `${env.DISPLAY}+${before.bounds.x},${before.bounds.y}`,
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
          const size = png.width * png.height * 3;
          while (pending.length >= size) {
            const offset = (3 * png.width + Math.round(png.width * 0.3)) * 3;
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
        const baseline = samples[inputFrame - 1];
        expect(
          Math.max(...samples.slice(inputFrame)) - baseline
        ).toBeGreaterThan(20);
        expect(samples.slice(-5)).toEqual(Array(5).fill(baseline));
        const active = samples
          .slice(inputFrame)
          .filter((value) => value > baseline + 2);
        expect(active.length).toBeGreaterThan(2);
        expect(active.length).toBeLessThan(22);
        const after = await readTerminalGridLayout(app);
        expect([after.columns, after.rows]).toEqual([
          layout.columns,
          layout.rows,
        ]);
        expect((await header.capture()).bounds.height).toBe(
          before.bounds.height
        );
        await evidence.captureEvidence('header-after-bell', () =>
          header.capture()
        );
      });
    });
  });
}
