// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { readWindowCellLayout, runGtkTest } from './gtk-test-helpers';
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

describe.concurrent('terminal selection presentation', () => {
  for (const backend of ['cairo', 'opengl']) {
    for (const rectangular of [false, true]) {
      it(`extends ${rectangular ? 'rectangular' : 'linear'} selection through history with the wheel (${backend})`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const peer = join(directory, 'history.mjs');
          const config = join(directory, 'history.ini');
          const lines = Array.from(
            { length: 100 },
            (_, row) => `${1000 + row} alpha beta`
          );
          await writeFile(
            peer,
            `process.stdin.setRawMode(true); process.stdin.resume();\nprocess.stdout.write(${JSON.stringify('\x1b[?25l\x1b[2J\x1b[H' + lines.join('\r\n'))});`
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
                expect(await status.text()).toContain(
                  `scroll=${100 - layout.rows}/${100 - layout.rows}`
                )
              );
              const bounds = (await terminal.capture()).bounds;
              const move = async (column: number, row: number) => {
                await app.input.moveMouseTo(
                  Math.trunc(bounds.x + column * layout.cellWidth),
                  Math.trunc(bounds.y + row * layout.cellHeight)
                );
              };
              const modifier = rectangular ? 'control' : 'shift';
              await app.input.setModifier(modifier, true);
              await move(0.1, layout.rows - 1.5);
              await app.input.setMouseButton('left', true);
              await move(3.9, 0.5);
              const steps = Math.floor(layout.rows / 3) + 4;
              await app.input.scrollWheel(0, -steps);
              const first = 100 - layout.rows - steps * 3;
              await waitForResult(async () => {
                expect(await status.text()).toContain(
                  `scroll=${first}/${100 - layout.rows}`
                );
                const current = await readWindowCellLayout(app);
                expect([current.cellWidth, current.cellHeight]).toEqual([
                  layout.cellWidth,
                  layout.cellHeight,
                ]);
                const frame = PNG.sync.read((await terminal.capture()).image);
                const offset =
                  (frame.width +
                    Math.trunc(layout.cellWidth * (rectangular ? 2 : 5) + 1)) *
                  4;
                expect(
                  frame.data.subarray(offset, offset + 3).toString('hex')
                ).toBe('b3b3b3');
              });
              await app.input.setMouseButton('left', false);
              await app.input.setModifier(modifier, false);
              await app.input.setModifier('control', true);
              await app.input.setModifier('shift', true);
              await app.input.pressKey('c');
              await app.input.setModifier('shift', false);
              await app.input.setModifier('control', false);
              const expected = lines
                .slice(first, 99)
                .map((line, index, selected) =>
                  rectangular
                    ? line.slice(0, 4)
                    : index === 0
                      ? line.slice(3)
                      : index === selected.length - 1
                        ? line.slice(0, 1)
                        : line
                )
                .join('\n');
              await waitForResult(async () => {
                const result = await execFileAsync(clipboardReader, [], {
                  env: await app.environment(),
                });
                expect(result.stdout).toBe(expected);
              });
              await evidence.captureEvidence('history-selection', async () =>
                terminal.capture()
              );
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
    it(`keeps selected blinking ink hidden while the fill stays visible in recorded frames (${backend})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const peer = join(directory, 'blink.mjs');
        const config = join(directory, 'blink.ini');
        await writeFile(
          peer,
          `process.stdin.setRawMode(true); process.stdin.resume();
process.stdout.write('\\x1b[?25l\\x1b[2J\\x1b[H\\x1b[5m██  ██\\r\\n\\x1b[38;2;0;0;0;48;2;179;179;179m██  ██');`
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
            const bounds = (await terminal.capture()).bounds;
            const width = layout.cellWidth;
            const height = layout.cellHeight;
            await waitForResult(async () => {
              const image = PNG.sync.read((await terminal.capture()).image);
              const offset =
                (Math.trunc(height * 1.5) * image.width +
                  Math.trunc(width * 2.5)) *
                4;
              expect(
                image.data.subarray(offset, offset + 3).toString('hex')
              ).toBe('b3b3b3');
            });
            await app.input.moveMouseTo(
              Math.trunc(bounds.x + 1),
              Math.trunc(bounds.y + height / 2)
            );
            await app.input.setMouseButton('left', true);
            await app.input.moveMouseTo(
              Math.trunc(bounds.x + width * 6 - 1),
              Math.trunc(bounds.y + height / 2)
            );
            await app.input.setMouseButton('left', false);
            await waitForResult(async () => {
              const image = PNG.sync.read((await terminal.capture()).image);
              const offset =
                (Math.trunc(height / 2) * image.width +
                  Math.trunc(width * 2.5)) *
                4;
              expect(
                image.data.subarray(offset, offset + 3).toString('hex')
              ).toBe('b3b3b3');
            });
            const videoWidth = width * 6;
            const videoHeight = height * 2;
            const frameSize = videoWidth * videoHeight * 3;
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
                `${videoWidth}x${videoHeight}`,
                '-i',
                `${(await app.environment()).DISPLAY}+${bounds.x},${bounds.y}`,
                '-map',
                '0:v',
                '-c:v',
                'ffv1',
                '-threads',
                '1',
                '-pix_fmt',
                'bgr0',
                join(evidence.directory, 'selection-blink.mkv'),
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
              { env: await app.environment(), stdio: ['pipe', 'pipe', 'pipe'] }
            );
            let pending = Buffer.alloc(0);
            let failure = '';
            let visible = 0;
            let hidden = 0;
            let transitions = 0;
            let last = '';
            const frames: string[] = [];
            let notify = (): void => {};
            const observed = new Promise<void>((resolve) => {
              notify = resolve;
            });
            const finished = new Promise<number | null>((resolve) => {
              recorder.once('error', (error) => {
                failure = error.message;
                notify();
                resolve(null);
              });
              recorder.once('close', (code) => {
                notify();
                resolve(code);
              });
            });
            recorder.stderr.on('data', (bytes: Buffer) => {
              failure += bytes.toString();
            });
            recorder.stdout.on('data', (bytes: Buffer) => {
              pending = Buffer.concat([pending, bytes]);
              while (pending.length >= frameSize) {
                const frame = pending.subarray(0, frameSize);
                pending = pending.subarray(frameSize);
                const pixel = (x: number, y: number): string => {
                  const offset =
                    (Math.trunc(y) * videoWidth + Math.trunc(x)) * 3;
                  return frame.subarray(offset, offset + 3).toString('hex');
                };
                const ink = pixel(width / 2, height / 2);
                const reference = pixel(width / 2, height * 1.5);
                if (
                  ink !== reference ||
                  pixel(width * 2.5, height / 2) !== 'b3b3b3'
                ) {
                  failure = `Selected blink differs from the unselected reference: ${ink}/${reference}`;
                  notify();
                }
                if (ink === '000000') visible++;
                if (ink === 'b3b3b3') hidden++;
                if (last && ink !== last) transitions++;
                last = ink;
                frames.push(ink);
                if (visible >= 2 && hidden >= 2 && transitions >= 2) notify();
              }
            });
            try {
              await observed;
            } finally {
              recorder.stdin.end('q\n');
              await finished;
            }
            await writeFile(
              join(evidence.directory, 'selection-blink-frames.json'),
              JSON.stringify(frames)
            );
            expect(failure).toBe('');
            expect(visible).toBeGreaterThanOrEqual(2);
            expect(hidden).toBeGreaterThanOrEqual(2);
            expect(transitions).toBeGreaterThanOrEqual(2);
          },
          {
            env: {
              GTK_IM_MODULE: 'gtk-im-context-simple',
              GDK_SCALE: '1',
              ...(backend === 'cairo'
                ? { GDK_GL: 'disable' }
                : { LIBGL_ALWAYS_SOFTWARE: '1' }),
            },
          }
        );
      });
    });
    for (const filter of [
      'none',
      'oklch',
      'reverse-lightness',
      'reverse-contrast',
      'reverse-roles',
      'reverse-cam16',
      'reverse-palette',
    ]) {
      for (const vision of ['none', 'protan', 'deutan', 'tritan']) {
        it(`fills both selection shapes and preserves concealed text (${backend}, ${filter}, ${vision})`, async (context) => {
          await withTemporaryDirectory(async (directory) => {
            const peer = join(directory, 'peer.mjs');
            const config = join(directory, 'selection.ini');
            const text = '██  ██  ██';
            const output =
              '\x1b[?25l\x1b[2J\x1b[H' +
              `\x1b[38;2;255;0;128;48;2;32;48;64m${text}\r\n` +
              `\x1b[38;2;0;0;0;48;2;179;179;179m${text}\r\n` +
              '\x1b[0m\x1b[8m' +
              text +
              '\x1b[0m';
            await writeFile(
              peer,
              `process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write(${JSON.stringify(output)});`
            );
            await writeFile(
              config,
              `[general]\nbackground=#202020\n` +
                `[terminal]\ncolor_filter=${filter}\ncolor_vision=${vision}\ncolor_vision_strength=100\n` +
                `[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=raw.log\nmode=raw\n` +
                `[local]\ncommand_line=${process.execPath} ${peer}\n`
            );
            await runGtkTest(
              context,
              ['-c', config],
              async (app, evidence) => {
                const layout = await waitForResult(async () => {
                  expect(
                    await readFile(join(directory, 'raw.log'), 'utf8')
                  ).toBe(output.replace(/\n/g, '\r\n'));
                  return await readWindowCellLayout(app);
                });
                await layout.mainWindow.activate();
                const terminal = await app.getById('terminal_view');
                const bounds = (await terminal.capture()).bounds;
                const width = layout.cellWidth;
                const height = layout.cellHeight;
                const select = async (
                  rectangular: boolean,
                  row: number
                ): Promise<void> => {
                  await app.input.setModifier('control', rectangular);
                  await app.input.moveMouseTo(
                    Math.trunc(bounds.x + width * 0.1),
                    Math.trunc(bounds.y + height * (row + 0.5))
                  );
                  await app.input.setMouseButton('left', true);
                  await app.input.moveMouseTo(
                    Math.trunc(bounds.x + width * 9.9),
                    Math.trunc(bounds.y + height * (row + 0.5))
                  );
                  await app.input.setMouseButton('left', false);
                  await app.input.setModifier('control', false);
                };
                const checkPixels = async (
                  row: number,
                  concealed: boolean
                ): Promise<void> => {
                  const frame = PNG.sync.read((await terminal.capture()).image);
                  const pixel = (x: number, y: number): string => {
                    const offset =
                      (Math.trunc(y) * frame.width + Math.trunc(x)) * 4;
                    return frame.data
                      .subarray(offset, offset + 3)
                      .toString('hex');
                  };
                  const background = pixel(width * 2.5, height * 1.5);
                  const foreground = pixel(width * 0.5, height * 1.5);
                  expect(background).not.toBe(foreground);
                  if (filter === 'none') {
                    expect(background).toBe('b3b3b3');
                    expect(foreground).toBe('000000');
                  }
                  for (let y = 2; y < height - 2; y++) {
                    for (let x = 2; x < width * 10 - 2; x++) {
                      const expected = concealed
                        ? background
                        : pixel(x, height + y);
                      expect(
                        pixel(x, height * row + y),
                        `pixel ${x},${y}`
                      ).toBe(expected);
                    }
                  }
                };
                for (const rectangular of [false, true]) {
                  await select(rectangular, 0);
                  await waitForResult(async () => checkPixels(0, false));
                  await app.input.setModifier('control', true);
                  await app.input.setModifier('shift', true);
                  await app.input.pressKey('c');
                  await app.input.setModifier('shift', false);
                  await app.input.setModifier('control', false);
                  await waitForResult(async () => {
                    const result = await execFileAsync(clipboardReader, [], {
                      env: await app.environment(),
                    });
                    expect(result.stdout).toBe(text);
                  });
                  // Opening the application menu moves focus away from the terminal.
                  const menu = expectElementKind(
                    await app.getById('application_menu_button'),
                    'toggleButton'
                  );
                  await menu.click();
                  await waitForResult(async () => checkPixels(0, false));
                  await app.input.pressKey('Escape');
                  await waitForResult(async () =>
                    expect(await menu.isChecked()).toBe(false)
                  );
                  await select(rectangular, 2);
                  await waitForResult(async () => checkPixels(2, true));
                }
                await evidence.captureEvidence('selection-conceal', async () =>
                  terminal.capture()
                );
                expect(await readFile(join(directory, 'raw.log'), 'utf8')).toBe(
                  output.replace(/\n/g, '\r\n')
                );
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
  }
});
