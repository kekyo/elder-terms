// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { withTemporaryDirectory } from '../../shared/tests/connection-test-helpers.ts';
import { spawn } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  expectWindowCellSize,
  readWindowCellLayout,
  runGtkTest,
  scrollWheelBurstWithControl,
  scrollWheelWithControl,
} from './gtk-test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

describe.concurrent('boxrig main window layout', () => {
  for (const backend of ['opengl', 'cairo']) {
    it(`keeps text visible and the PTY grid stable throughout zoom and resize (${backend})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const peerPath = join(directory, 'peer.mjs');
        const sizesPath = join(directory, 'sizes.jsonl');
        const inputPath = join(directory, 'input.txt');
        const configPath = join(directory, 'terminal.ini');
        await writeFile(
          peerPath,
          `
import { appendFileSync } from 'node:fs';
const report = () => appendFileSync(process.argv[2], JSON.stringify({
  columns: process.stdout.columns, rows: process.stdout.rows,
}) + '\\n');
process.stdout.on('resize', report);
report();
process.stdin.setRawMode(true);
process.stdin.resume();
process.stdin.on('data', (bytes) => appendFileSync(process.argv[3], bytes));
process.stdout.write('\\x1b[?25l\\x1b[2J\\x1b[H\\x1b[38;2;255;0;255mZOOM-STABLE');
`,
          'utf8'
        );
        await writeFile(
          configPath,
          `[general]\nauto_close=false\n[terminal]\nwidth=80\nheight=18\nfont_size_dip=14.5\nscrollback_lines=1234\n[local]\ncommand_line=${process.execPath} ${peerPath} ${sizesPath} ${inputPath}\n`,
          'utf8'
        );
        await runGtkTest(
          context,
          ['-c', configPath],
          async (app, evidence) => {
            const readSizes = async (): Promise<
              { columns: number; rows: number }[]
            > =>
              (await readFile(sizesPath, 'utf8'))
                .trim()
                .split('\n')
                .map((line) => JSON.parse(line));
            const initial = await waitForResult(async () => {
              expect(await readSizes()).toEqual([{ columns: 80, rows: 18 }]);
              const status = await app.getById('fixture_terminal_state_label');
              if (status.kind !== 'label')
                throw new Error('Missing native presentation state');
              expect(await status.text()).toContain(`backend=${backend}`);
              expect(await status.text()).toContain('history=1234');
              const layout = await readWindowCellLayout(app);
              expectWindowCellSize(layout, 80, 18);
              expect(layout.fontSizeDip).toBe(14.5);
              return layout;
            });
            const terminal = await app.getById('terminal_view');
            const bounds = (await terminal.capture()).bounds;
            await app.input.moveMouseTo(bounds.x + 150, bounds.y + 30);
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await app.input.pressKey('a');
            await waitForResult(async () => {
              expect(await readFile(inputPath, 'utf8')).toBe('a');
            });
            const screen = PNG.sync.read((await app.capture()).image);
            const env = await app.environment();
            const frameSize = screen.width * screen.height * 4;
            let pending = Buffer.alloc(0);
            let frameNumber = 0;
            let armed = false;
            let failure = '';
            const samples: { frame: number; ink: number }[] = [];
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
                `${screen.width}x${screen.height}`,
                '-i',
                env.DISPLAY ?? '',
                '-map',
                '0:v',
                '-c:v',
                'ffv1',
                '-threads',
                '1',
                '-pix_fmt',
                'bgr0',
                '-fps_mode',
                'passthrough',
                join(evidence.directory, 'zoom-resize.mkv'),
                '-map',
                '0:v',
                '-c:v',
                'rawvideo',
                '-threads',
                '1',
                '-pix_fmt',
                'bgr0',
                '-fps_mode',
                'passthrough',
                '-f',
                'rawvideo',
                'pipe:1',
              ],
              { env, stdio: ['pipe', 'pipe', 'pipe'] }
            );
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
                const frame = pending.subarray(0, frameSize);
                pending = pending.subarray(frameSize);
                frameNumber += 1;
                let ink = 0;
                for (let y = bounds.y; y < bounds.y + 45; y += 1) {
                  for (let x = bounds.x; x < bounds.x + 180; x += 1) {
                    const index = (y * screen.width + x) * 4;
                    if (
                      frame[index] > 150 &&
                      frame[index + 1] < 100 &&
                      frame[index + 2] > 150
                    )
                      ink += 1;
                  }
                }
                if (armed) samples.push({ frame: frameNumber, ink });
                else if (ink > 50) armed = true;
              }
            });
            const states: unknown[] = [];
            try {
              await waitForResult(async () => {
                expect(failure).toBe('');
                expect(armed).toBe(true);
              });
              let previous = initial;
              for (const step of [-1, -1, -1, 1, 1, 1]) {
                const before = frameNumber;
                await scrollWheelWithControl(app, step);
                const next = await waitForResult(async () => {
                  const layout = await readWindowCellLayout(app);
                  expect(layout.fontSizeDip).not.toBe(previous.fontSizeDip);
                  expectWindowCellSize(layout, 80, 18);
                  expect(frameNumber).toBeGreaterThan(before);
                  return layout;
                });
                states.push({
                  frame: frameNumber,
                  font: next.fontSizeDip,
                  columns: next.columns,
                  rows: next.rows,
                });
                previous = next;
              }
              expect(previous.fontSizeDip).toBe(initial.fontSizeDip);
              expect(await readSizes()).toEqual([{ columns: 80, rows: 18 }]);
              await scrollWheelBurstWithControl(app, [-1, -1, -1, 1, 1, 1]);
              // Input acknowledgement establishes that GTK has processed the
              // injected burst before we issue a separate window allocation.
              await app.input.pressKey('x');
              await waitForResult(async () => {
                expect(await readFile(inputPath, 'utf8')).toBe('ax');
                const layout = await readWindowCellLayout(app);
                expect(layout.fontSizeDip).toBe(initial.fontSizeDip);
                expectWindowCellSize(layout, 80, 18);
              });
              const beforeResize = frameNumber;
              await initial.mainWindow.resizeTo(
                initial.mainBounds.width + 4 * initial.cellWidth,
                initial.mainBounds.height + 2 * initial.cellHeight
              );
              await waitForResult(async () => {
                const layout = await readWindowCellLayout(app);
                expectWindowCellSize(layout, 84, 20);
                expect(layout.fontSizeDip).toBe(initial.fontSizeDip);
                expect(await readSizes()).toEqual([
                  { columns: 80, rows: 18 },
                  { columns: 84, rows: 20 },
                ]);
                expect(frameNumber).toBeGreaterThan(beforeResize);
              });
              await evidence.captureEvidence('final-terminal', async () =>
                terminal.capture()
              );
            } finally {
              recorder.stdin.write('q\n');
              const code = await finished;
              await writeFile(
                join(evidence.directory, 'layout-timeline.json'),
                JSON.stringify(
                  { states, sizes: await readSizes(), samples },
                  null,
                  2
                )
              );
              expect(code, failure).toBe(0);
            }
            expect(samples.length).toBeGreaterThan(6);
            expect(
              samples.every(({ ink }) => ink > 50),
              JSON.stringify(samples)
            ).toBe(true);
          },
          {
            env: {
              LIBGL_ALWAYS_SOFTWARE: '1',
              GTK_IM_MODULE: 'gtk-im-context-simple',
              ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
            },
          }
        );
      });
    });
  }
});
