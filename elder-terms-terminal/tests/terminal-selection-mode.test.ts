// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { readWindowCellLayout, runGtkTest } from './gtk-test-helpers.ts';
import {
  withTemporaryDirectory,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

const execFileAsync = promisify(execFile);
const clipboardReader = fileURLToPath(
  new URL(
    '../../.build/elder-terms-terminal/clipboard-read-helper',
    import.meta.url
  )
);

describe.concurrent('selection mode motion video', () => {
  for (const backend of ['cairo', 'opengl']) {
    for (const rectangular of [false, true]) {
      it(`switches only after motion from ${rectangular ? 'rectangle' : 'linear'} (${backend})`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const peer = join(directory, 'mode.mjs');
          const config = join(directory, 'mode.ini');
          await writeFile(
            peer,
            `process.stdin.setRawMode(true); process.stdin.resume();\nprocess.stdout.write(${JSON.stringify('\x1b[?25l\x1b[2J\x1b[Habcdefgh\r\nijklmnop\r\nqrstuvwx')});`
          );
          await writeFile(
            config,
            `[local]\ncommand_line=${process.execPath} ${peer}\n`
          );
          await runGtkTest(
            context,
            ['-c', config],
            async (app, evidence) => {
              const layout = await waitForResult(async () =>
                readWindowCellLayout(app)
              );
              await layout.mainWindow.activate();
              const terminal = await app.getById('terminal_view');
              const status = expectElementKind(
                await app.getById('fixture_terminal_state_label'),
                'label'
              );
              await waitForResult(async () =>
                expect(await status.text()).toContain('ready=1')
              );
              const bounds = (await terminal.capture()).bounds;
              const width = layout.cellWidth * 12;
              const height = layout.cellHeight * 3;
              const frameSize = width * height * 3;
              const env = await app.environment();
              const recorder = spawn(
                'ffmpeg',
                [
                  '-y',
                  '-hide_banner',
                  '-loglevel',
                  'error',
                  '-f',
                  'x11grab',
                  '-framerate',
                  '30',
                  '-draw_mouse',
                  '0',
                  '-video_size',
                  `${width}x${height}`,
                  '-i',
                  `${env.DISPLAY}+${bounds.x},${bounds.y}`,
                  '-map',
                  '0:v',
                  '-c:v',
                  'ffv1',
                  '-threads',
                  '1',
                  '-pix_fmt',
                  'bgr0',
                  join(evidence.directory, 'selection-mode.mkv'),
                  '-map',
                  '0:v',
                  '-c:v',
                  'rawvideo',
                  '-threads',
                  '1',
                  '-pix_fmt',
                  'rgb24',
                  '-f',
                  'rawvideo',
                  'pipe:1',
                ],
                { env, stdio: ['pipe', 'pipe', 'pipe'] }
              );
              let pending = Buffer.alloc(0);
              let failure = '';
              const frames: string[] = [];
              const observations: {
                label: string;
                frame: number;
                hash: string;
              }[] = [];
              const finished = new Promise<number | null>((resolve) => {
                recorder.once('error', (error) => {
                  failure = error.message;
                  resolve(null);
                });
                recorder.once('close', resolve);
              });
              recorder.stderr.on('data', (bytes: Buffer) => {
                failure += bytes.toString();
              });
              recorder.stdout.on('data', (bytes: Buffer) => {
                pending = Buffer.concat([pending, bytes]);
                while (pending.length >= frameSize) {
                  frames.push(
                    createHash('sha256')
                      .update(pending.subarray(0, frameSize))
                      .digest('hex')
                  );
                  pending = pending.subarray(frameSize);
                }
              });
              const move = async (column: number) => {
                await app.input.moveMouseTo(
                  Math.trunc(bounds.x + column * layout.cellWidth),
                  Math.trunc(bounds.y + 2.5 * layout.cellHeight)
                );
              };
              const observe = async (
                label: string,
                shape: boolean | undefined,
                unchanged: string | undefined
              ) => {
                const start = frames.length;
                const observed = await waitForResult(async () => {
                  expect(failure).toBe('');
                  expect(await status.text()).toContain('ready=1');
                  const image = PNG.sync.read((await terminal.capture()).image);
                  const rgb = Buffer.alloc(frameSize);
                  for (let y = 0; y < height; y++)
                    for (let x = 0; x < width; x++) {
                      const from = (y * image.width + x) * 4;
                      image.data.copy(rgb, (y * width + x) * 3, from, from + 3);
                    }
                  if (shape !== undefined) {
                    const inside =
                      ((layout.cellHeight + 1) * width +
                        layout.cellWidth * 2 +
                        1) *
                      3;
                    const outside =
                      ((layout.cellHeight + 1) * width +
                        layout.cellWidth * 6 +
                        1) *
                      3;
                    expect(
                      rgb.subarray(inside, inside + 3).toString('hex')
                    ).toBe('b3b3b3');
                    expect(
                      rgb.subarray(outside, outside + 3).toString('hex') ===
                        'b3b3b3'
                    ).toBe(!shape);
                  }
                  const hash = createHash('sha256').update(rgb).digest('hex');
                  expect(frames.length).toBeGreaterThan(start + 6);
                  expect(frames[frames.length - 1]).toBe(hash);
                  if (unchanged !== undefined) {
                    expect(hash).toBe(unchanged);
                    expect(
                      frames.slice(start).every((frame) => frame === unchanged)
                    ).toBe(true);
                  }
                  return { label, frame: frames.length - 1, hash };
                });
                observations.push(observed);
                await evidence.captureEvidence(label, async () =>
                  terminal.capture()
                );
                return observed;
              };
              try {
                const before = await observe('before', undefined, undefined);
                await app.input.setModifier('control', rectangular);
                await app.input.moveMouseTo(
                  bounds.x + 1,
                  Math.trunc(bounds.y + layout.cellHeight * 0.5)
                );
                await app.input.setMouseButton('left', true);
                await move(3.9);
                const initial = await observe(
                  'initial-drag',
                  rectangular,
                  undefined
                );
                await app.input.setModifier('control', !rectangular);
                await observe('ctrl-only', rectangular, initial.hash);
                await move(3.6);
                const changed = await observe(
                  'motion-switch',
                  !rectangular,
                  undefined
                );
                expect(changed.hash).not.toBe(initial.hash);
                await app.input.setModifier('control', rectangular);
                await observe('ctrl-only-return', !rectangular, changed.hash);
                await move(3.9);
                const restored = await observe(
                  'motion-return',
                  rectangular,
                  undefined
                );
                expect(restored.hash).toBe(initial.hash);
                await app.input.setModifier('control', !rectangular);
                await app.input.setMouseButton('left', false);
                const released = await observe(
                  'released',
                  rectangular,
                  initial.hash
                );
                // Every recorded intermediate frame must show one complete
                // selection, including the frames between input checkpoints.
                expect(
                  frames
                    .slice(initial.frame, released.frame + 1)
                    .every(
                      (hash) => hash === initial.hash || hash === changed.hash
                    )
                ).toBe(true);
                await app.input.setModifier('control', true);
                await app.input.setModifier('shift', true);
                await app.input.pressKey('c');
                await app.input.setModifier('shift', false);
                await app.input.setModifier('control', false);
                await waitForResult(async () => {
                  const result = await execFileAsync(clipboardReader, [], {
                    env,
                  });
                  expect(result.stdout).toBe(
                    rectangular
                      ? 'abcd\nijkl\nqrst'
                      : 'abcdefgh\nijklmnop\nqrst'
                  );
                });
                await app.input.pressKey('Escape');
                await observe('cleared', undefined, before.hash);
                await writeFile(
                  join(evidence.directory, 'selection-mode-frames.json'),
                  JSON.stringify({ observations, frames }, null, 2)
                );
              } finally {
                await app.input.setMouseButton('left', false);
                await app.input.setModifier('control', false);
                await app.input.setModifier('shift', false);
                if (recorder.exitCode === null && recorder.signalCode === null)
                  recorder.stdin.write('q\n');
                expect(await finished, failure).toBe(0);
              }
            },
            {
              env: {
                GTK_IM_MODULE: 'gtk-im-context-simple',
                ...(backend === 'cairo'
                  ? { GDK_GL: 'disable' }
                  : { LIBGL_ALWAYS_SOFTWARE: '1' }),
              },
            }
          );
        });
      });
    }
  }
});
