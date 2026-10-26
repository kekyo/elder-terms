// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmod, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  defaultRows,
  readWindowCellLayout,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

describe.concurrent('terminal selection autoscroll video', () => {
  for (const [backend, footerRows] of [
    ['cairo', 0],
    ['opengl', 0],
    ['cairo', 3],
    ['opengl', 3],
  ] as const) {
    for (const rectangular of [false, true]) {
      it(`moves and stops a ${rectangular ? 'rectangular' : 'linear'} selection with ${footerRows === 0 ? 'full screen' : 'top region'} history (${backend})`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const peer = join(directory, 'scroll.mjs');
          const config = join(directory, 'scroll.ini');
          const action = join(directory, 'xdg-open');
          const actions = join(directory, 'actions.jsonl');
          await writeFile(actions, '');
          await writeFile(
            action,
            `#!${process.execPath}\nimport { appendFileSync } from 'node:fs';\nappendFileSync(${JSON.stringify(actions)}, JSON.stringify(process.argv.slice(2)) + '\\n');\n`
          );
          await chmod(action, 0o755);
          const lines = Array.from(
            { length: 200 },
            (_, row) => `${1000 + row} alpha beta`
          );
          lines[199] =
            '\x1b]8;;https://selection.test/last\x1b\\' +
            lines[199] +
            '\x1b]8;;\x1b\\';
          const contentRows = defaultRows - footerRows;
          // Keep an input area below a top-anchored scrolling region, as
          // inline TUIs do when inserting output above their composer.
          const output =
            '\x1b[?25l\x1b[2J\x1b[H' +
            (footerRows === 0
              ? ''
              : `\x1b[${contentRows + 1};1Hprompt> \x1b[1;${contentRows}r`) +
            lines.join('\r\n') +
            '\x1b[r';
          await writeFile(
            peer,
            `process.stdin.setRawMode(true); process.stdin.resume();\nprocess.stdout.write(${JSON.stringify(output)});`
          );
          await writeFile(
            config,
            `[local]\ncommand_line=${process.execPath} ${peer}\n[hyperlink]\nenabled=true\n`
          );
          await runGtkTest(
            context,
            ['-c', config],
            async (app, evidence) => {
              const layout = await waitForResult(async () =>
                readWindowCellLayout(app)
              );
              expect(layout.rows).toBe(defaultRows);
              await layout.mainWindow.activate();
              const terminal = await app.getById('terminal_view');
              const status = expectElementKind(
                await app.getById('fixture_terminal_state_label'),
                'label'
              );
              const state = async () => {
                const report = await status.text();
                const scroll = /scroll=(\d+)\/(\d+)/.exec(report);
                if (!scroll) throw new Error('Missing scroll observation');
                return {
                  position: Number(scroll[1]),
                  history: Number(scroll[2]),
                  ready: report.includes('ready=1'),
                  selected: report.includes('selected=1'),
                  rows: Number(/selection_rows=(\d+)/.exec(report)?.[1] ?? 0),
                };
              };
              const initial = await waitForResult(async () => {
                const value = await state();
                expect(value.history).toBe(200 - contentRows);
                expect(value.ready).toBe(true);
                return value;
              });
              const bounds = (await terminal.capture()).bounds;
              const width = layout.cellWidth * 14;
              const height = layout.cellHeight * 8;
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
                  join(evidence.directory, 'autoscroll.mkv'),
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
              const frames: { hash: string; gray: number }[] = [];
              const observations: {
                label: string;
                frame: number;
                position: number;
                rows: number;
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
                  const frame = pending.subarray(0, frameSize);
                  pending = pending.subarray(frameSize);
                  let gray = 0;
                  for (let offset = 0; offset < frame.length; offset += 3)
                    if (
                      frame[offset] === 179 &&
                      frame[offset + 1] === 179 &&
                      frame[offset + 2] === 179
                    )
                      gray++;
                  frames.push({
                    hash: createHash('sha256').update(frame).digest('hex'),
                    gray,
                  });
                }
              });
              const move = async (column: number, row: number) => {
                await app.input.moveMouseTo(
                  Math.trunc(bounds.x + column * layout.cellWidth),
                  Math.trunc(bounds.y + row * layout.cellHeight)
                );
              };
              const observe = async (label: string) => {
                const observed = await waitForResult(async () => {
                  expect(failure).toBe('');
                  const current = await state();
                  expect(current.ready).toBe(true);
                  const image = PNG.sync.read((await terminal.capture()).image);
                  const rgb = Buffer.alloc(frameSize);
                  for (let y = 0; y < height; y++)
                    for (let x = 0; x < width; x++) {
                      const from = (y * image.width + x) * 4;
                      const to = (y * width + x) * 3;
                      image.data.copy(rgb, to, from, from + 3);
                    }
                  const hash = createHash('sha256').update(rgb).digest('hex');
                  expect(frames[frames.length - 1]?.hash).toBe(hash);
                  expect((await state()).position).toBe(current.position);
                  return {
                    label,
                    frame: frames.length - 1,
                    position: current.position,
                    rows: current.rows,
                    hash,
                  };
                });
                observations.push(observed);
                await evidence.captureEvidence(label, async () =>
                  terminal.capture()
                );
                return observed;
              };
              const modifier = rectangular ? 'control' : 'shift';
              try {
                const before = await observe('before');
                await move(7.5, 0.5);
                await app.input.scrollWheel(0, -2);
                await waitForResult(async () =>
                  expect((await state()).position).toBe(initial.position - 6)
                );
                await waitForResult(async () =>
                  expect(frames[frames.length - 1]?.hash).not.toBe(before.hash)
                );
                const scrolled = await observe('wheel-up');
                expect(scrolled.hash).not.toBe(before.hash);
                await app.input.scrollWheel(0, 2);
                await waitForResult(async () =>
                  expect((await state()).position).toBe(initial.position)
                );
                await waitForResult(async () =>
                  expect(frames[frames.length - 1]?.hash).toBe(before.hash)
                );
                expect((await observe('wheel-down')).hash).toBe(before.hash);
                // Run the complete product workflow before the video checks:
                // normal selection, Escape, link click, rectangle from that
                // link, and a wheel extension with Ctrl still held.
                await move(0.1, 0.5);
                await app.input.setMouseButton('left', true);
                await move(3.9, 1.5);
                await app.input.setMouseButton('left', false);
                await waitForResult(async () =>
                  expect((await state()).selected).toBe(true)
                );
                await app.input.pressKey('Escape');
                await waitForResult(async () =>
                  expect((await state()).selected).toBe(false)
                );
                await app.input.setModifier('control', true);
                await move(7.5, contentRows - 0.5);
                await app.input.setMouseButton('left', true);
                await app.input.setMouseButton('left', false);
                const expectedAction =
                  JSON.stringify(['https://selection.test/last']) + '\n';
                await waitForResult(async () =>
                  expect(await readFile(actions, 'utf8')).toBe(expectedAction)
                );
                await move(0.1, contentRows - 0.5);
                await app.input.setMouseButton('left', true);
                await move(3.9, 0.5);
                await app.input.scrollWheel(0, -2);
                await waitForResult(async () => {
                  const current = await state();
                  expect(current.position).toBe(initial.position - 6);
                  expect(current.rows).toBeGreaterThan(layout.rows);
                });
                await app.input.setMouseButton('left', false);
                await app.input.setModifier('control', false);
                await app.input.pressKey('Escape');
                await waitForResult(async () =>
                  expect((await state()).selected).toBe(false)
                );
                await app.input.scrollWheel(0, 2);
                await waitForResult(async () =>
                  expect((await state()).position).toBe(initial.position)
                );
                expect(await readFile(actions, 'utf8')).toBe(expectedAction);
                await app.input.setModifier(modifier, true);
                await move(0.1, contentRows - 1.5);
                await app.input.setMouseButton('left', true);
                await move(3.9, -2.5);
                await waitForResult(async () =>
                  expect((await state()).position).toBeLessThan(
                    initial.position - 12
                  )
                );
                await move(3.9, 0.5);
                const up = await observe('up');
                expect(frames[up.frame]?.gray).toBeGreaterThan(
                  (width * height) / 10
                );
                expect(up.position).toBeLessThan(initial.position - 12);
                const beforeMore = frames.length;
                await move(rectangular ? 0.1 : 3.9, -2.5);
                await waitForResult(async () => {
                  const current = await state();
                  expect(current.position).toBeLessThan(up.position - 6);
                  if (rectangular) expect(current.rows).toBe(0);
                });
                await move(rectangular ? 0.1 : 3.9, 0.5);
                const more = await observe(
                  rectangular ? 'zero-width' : 'further-up'
                );
                expect(more.position).toBeLessThan(up.position - 6);
                expect(
                  new Set(
                    frames
                      .slice(beforeMore, more.frame + 1)
                      .map((frame) => frame.hash)
                  ).size
                ).toBeGreaterThan(2);
                // Antialiased text can contain isolated #b3b3b3 pixels; an
                // empty rectangle has no filled area of that color.
                if (rectangular)
                  expect(frames[more.frame]?.gray).toBeLessThan(
                    (width * height) / 100
                  );
                await move(3.9, -2.5);
                await waitForResult(async () => {
                  const current = await state();
                  expect(current.position).toBeLessThan(more.position - 6);
                  if (rectangular)
                    expect(current.rows).toBeGreaterThan(layout.rows);
                });
                await move(3.9, 0.5);
                const restored = await observe('expanded');
                expect(frames[restored.frame]?.gray).toBeGreaterThan(
                  (width * height) / 10
                );
                await move(3.9, layout.rows + 2.5);
                await waitForResult(async () =>
                  expect((await state()).position).toBeGreaterThan(
                    restored.position + 6
                  )
                );
                await app.input.setMouseButton('left', false);
                await app.input.setModifier(modifier, false);
                const stopped = await observe('released');
                await waitForResult(async () => {
                  expect(frames.length).toBeGreaterThan(stopped.frame + 10);
                  expect((await state()).position).toBe(stopped.position);
                  expect(
                    frames
                      .slice(stopped.frame, stopped.frame + 10)
                      .every((frame) => frame.hash === stopped.hash)
                  ).toBe(true);
                });
                expect(stopped.position).toBeGreaterThan(restored.position + 6);
                await app.input.pressKey('Escape');
                await waitForResult(async () =>
                  expect((await state()).selected).toBe(false)
                );
                const cleared = await observe('cleared');
                expect(frames[cleared.frame]?.gray).toBeLessThan(
                  (width * height) / 100
                );
                expect(await readFile(actions, 'utf8')).toBe(expectedAction);
                await writeFile(
                  join(evidence.directory, 'autoscroll-frames.json'),
                  JSON.stringify({ observations, frames }, null, 2)
                );
              } finally {
                await app.input.setMouseButton('left', false);
                await app.input.setModifier(modifier, false);
                if (recorder.exitCode === null && recorder.signalCode === null)
                  recorder.stdin.write('q\n');
                expect(await finished, failure).toBe(0);
              }
            },
            {
              env: {
                PATH: `${directory}:${process.env.PATH ?? ''}`,
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
