// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  runGtkTest,
  withTemporaryDirectory,
  readTerminalGridLayout,
  pressKeyWithModifiers,
} from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const exec = promisify(execFile);
const samples = ['①のフォントサイズ', 'A①B', '①①', '①界'];
const clipboardReader = fileURLToPath(
  new URL(
    '../../.build/elder-terms-terminal/clipboard-read-helper',
    import.meta.url
  )
);

interface CursorReport {
  pid: number;
  phase: number;
  positions: number[][];
}

for (const backend of ['cairo', 'opengl']) {
  for (const span of [2, 3]) {
    it(`records a full-width block cursor on every occupied column (${backend} ${span} columns)`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const path = join(directory, 'cursor.ini');
        const peer = join(directory, 'cursor.mjs');
        const ready = join(directory, 'ready');
        await writeFile(
          peer,
          String.raw`
import { writeFileSync } from 'node:fs';
process.stdin.setRawMode(true);
process.stdin.setEncoding('utf8');
process.stdin.resume();
process.stdout.write('\x1b]12;#ffcc00\x07\x1b[?25l\x1b[38;2;255;255;255m\x1b[48;2;0;0;0m\x1b[2J\x1b[HA${span === 3 ? '①' : '界'}B\x1b[6n');
let input = '';
process.stdin.on('data', chunk => {
  input += chunk;
  if (input.includes('\x1b[1;${span + 3}R')) {
    writeFileSync(${JSON.stringify(ready)}, 'ready');
    input = '';
  }
  for (const key of chunk) {
    const column = { l: 2, t: 3, s: 4, n: ${span + 2} }[key];
    if (column) process.stdout.write('\x1b[1;' + column + 'H\x1b[?25h');
  }
});
`
        );
        await writeFile(
          path,
          `[terminal]\nambiguous_width=${span === 3 ? 'font' : 'narrow'}\n${span === 3 ? 'font_families=Boxrig Width Fixture;\nfont_size_dip=20\n' : ''}cursor_shape=block\ncursor_blink=true\n[local]\ncommand_line=${process.execPath} ${peer}\n`
        );
        await runGtkTest(
          context,
          ['-c', path],
          async (app, evidence) => {
            await waitForResult(async () =>
              expect(await readFile(ready, 'utf8')).toBe('ready')
            );
            const layout = await readTerminalGridLayout(app);
            const terminal = await app.getById('terminal_view');
            const width = layout.cellWidth * (span + 2);
            const height = layout.cellHeight;
            await app.input.moveMouseTo(
              layout.terminalCapture.bounds.x + 50,
              layout.terminalCapture.bounds.y + 50
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            const baseline = await waitForResult(async () => {
              const png = PNG.sync.read((await terminal.capture()).image);
              const rgb = Buffer.alloc(width * height * 3);
              let ink = 0;
              for (let y = 0; y < height; y++)
                for (let x = 0; x < width; x++) {
                  const from = (y * png.width + x) * 4;
                  const to = (y * width + x) * 3;
                  png.data.copy(rgb, to, from, from + 3);
                  if (rgb[to] > 100) ink++;
                  // Native font antialiasing can give white text colored edge pixels.
                  expect(
                    rgb[to] > 200 && rgb[to + 1] > 150 && rgb[to + 2] < 50
                  ).toBe(false);
                }
              expect(ink).toBeGreaterThan(0);
              return rgb;
            });
            const env = await app.environment();
            for (const [key, column, occupied] of [
              ['l', 1, span],
              ['t', 1, span],
              ...(span === 3 ? [['s', 1, span] as const] : []),
              ['n', span + 1, 1],
            ] as const) {
              await app.input.pressKey(key);
              await waitForResult(async () => {
                const png = PNG.sync.read((await terminal.capture()).image);
                const offset = column * layout.cellWidth * 4;
                expect([...png.data.subarray(offset, offset + 3)]).toEqual([
                  255, 204, 0,
                ]);
              });
              const video = join(evidence.directory, `wide-cursor-${key}.mkv`);
              await exec(
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
                  `${env.DISPLAY}+${layout.terminalCapture.bounds.x},${layout.terminalCapture.bounds.y}`,
                  '-frames:v',
                  '90',
                  '-c:v',
                  'ffv1',
                  '-threads',
                  '1',
                  video,
                ],
                { env }
              );
              const raw = await exec(
                'ffmpeg',
                [
                  '-hide_banner',
                  '-loglevel',
                  'error',
                  '-i',
                  video,
                  '-f',
                  'rawvideo',
                  '-pix_fmt',
                  'rgb24',
                  'pipe:1',
                ],
                { encoding: 'buffer', maxBuffer: 4 * 1024 * 1024 }
              );
              const stride = width * height * 3;
              expect(raw.stdout.length).toBe(90 * stride);
              let visible = 0,
                hidden = 0;
              for (let start = 0; start < raw.stdout.length; start += stride) {
                const frame = raw.stdout.subarray(start, start + stride);
                let left = width,
                  right = -1;
                for (let y = 0; y < height; y++)
                  for (let x = 0; x < width; x++) {
                    const offset = (y * width + x) * 3;
                    if (
                      frame[offset] > 200 &&
                      frame[offset + 1] > 150 &&
                      frame[offset + 2] < 50
                    ) {
                      left = Math.min(left, x);
                      right = Math.max(right, x);
                    }
                    if (
                      x < column * layout.cellWidth ||
                      x >= (column + occupied) * layout.cellWidth
                    )
                      expect(
                        frame
                          .subarray(offset, offset + 3)
                          .equals(baseline.subarray(offset, offset + 3))
                      ).toBe(true);
                  }
                if (right < 0) {
                  hidden++;
                  expect(
                    frame.equals(baseline),
                    'blink restores the underlying glyphs'
                  ).toBe(true);
                } else {
                  visible++;
                  expect(left).toBe(column * layout.cellWidth);
                  expect(right).toBe(
                    (column + occupied) * layout.cellWidth - 1
                  );
                }
              }
              expect(visible).toBeGreaterThan(0);
              expect(hidden).toBeGreaterThan(0);
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
    }, 90_000);
  }
}

for (const backend of ['cairo', 'opengl']) {
  for (const initial of ['narrow', 'wide', 'font']) {
    it(`preserves active ambiguous width and applies saved width to a new terminal (${backend} ${initial})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const path = join(directory, 'width.ini');
        const peer = join(directory, 'width.mjs');
        const report = join(directory, 'cursor.json');
        await writeFile(
          peer,
          String.raw`
import { writeFileSync } from 'node:fs';
const path = ${JSON.stringify(report)};
const samples = ${JSON.stringify(samples)};
let input = '', phase = 0, colored = false, positions = [];
process.stdin.setRawMode(true);
process.stdin.setEncoding('utf8');
process.stdin.resume();
const draw = () => {
  positions = [];
  process.stdout.write('\x1b[?25l\x1b[0m\x1b[37;40m\x1b[2J');
  samples.forEach((sample, row) => {
    const text = colored ? sample.replace('①', '\x1b[31m①\x1b[37m') : sample;
    process.stdout.write('\x1b[' + (row + 1) + ';1H' + text + '\x1b[6n');
  });
};
process.stdin.on('data', chunk => {
  input += chunk;
  let match;
  while ((match = /\x1b\[(\d+);(\d+)R/.exec(input))) {
    positions.push([Number(match[1]), Number(match[2])]);
    input = input.slice(0, match.index) + input.slice(match.index + match[0].length);
    if (positions.length === samples.length)
      writeFileSync(path, JSON.stringify({pid: process.pid, phase, positions}));
  }
  if (input.includes('c') || input.includes('r')) {
    if (input.includes('c')) colored = !colored;
    input = ''; phase++; draw();
  }
});
writeFileSync(path, JSON.stringify({pid: process.pid, phase, positions}));
draw();
`
        );
        await writeFile(
          path,
          `[local]\ncommand_line=${process.execPath} ${peer}\n`
        );
        let firstPid = 0;
        const saved =
          initial === 'narrow'
            ? 'wide'
            : initial === 'wide'
              ? 'font'
              : 'narrow';
        for (const reloaded of [false, true]) {
          const active = reloaded ? saved : initial;
          const width = active === 'font' ? 3 : active === 'wide' ? 2 : 1;
          const expectedColumns = [
            17 + width,
            3 + width,
            1 + 2 * width,
            3 + width,
          ];
          await runGtkTest(
            context,
            ['-c', path],
            async (app, evidence) => {
              const readReport = async (phase: number): Promise<CursorReport> =>
                waitForResult(async () => {
                  const result = JSON.parse(
                    await readFile(report, 'utf8')
                  ) as CursorReport;
                  if (reloaded) expect(result.pid).not.toBe(firstPid);
                  expect(result.phase).toBe(phase);
                  expect(result.positions).toEqual(
                    expectedColumns.map((column, row) => [row + 1, column])
                  );
                  return result;
                });
              const started = await readReport(0);
              if (!reloaded) firstPid = started.pid;
              const layout = await readTerminalGridLayout(app);
              const terminal = await app.getById('terminal_view');
              const state = expectElementKind(
                await app.getById('fixture_terminal_state_label'),
                'label'
              );
              const focus = async (): Promise<void> => {
                await app.input.moveMouseTo(
                  layout.terminalCapture.bounds.x + 10,
                  layout.terminalCapture.bounds.y + 10
                );
                await app.input.setMouseButton('left', true);
                await app.input.setMouseButton('left', false);
              };
              await focus();
              const capture = async (): Promise<PNG> => {
                expect(await state.text()).toContain('ready=1');
                return PNG.sync.read((await terminal.capture()).image);
              };
              const before = await waitForResult(async () => {
                const pixels = await capture();
                for (let row = 0; row < samples.length; row++) {
                  let ink = 0;
                  for (
                    let y = row * layout.cellHeight;
                    y < (row + 1) * layout.cellHeight;
                    y++
                  )
                    for (
                      let x = 0;
                      x < (expectedColumns[row] - 1) * layout.cellWidth;
                      x++
                    )
                      if (pixels.data[(y * pixels.width + x) * 4] > 100) ink++;
                  expect(ink).toBeGreaterThan(0);
                }
                return pixels;
              });
              if (active === 'font') {
                // Both fixture glyphs have a 2.1-cell advance, centered in 3 cells.
                for (const column of [0, 3]) {
                  let left = (column + 3) * layout.cellWidth;
                  let right = column * layout.cellWidth - 1;
                  for (
                    let y = 2 * layout.cellHeight;
                    y < 3 * layout.cellHeight;
                    y++
                  ) {
                    for (
                      let x = column * layout.cellWidth;
                      x < (column + 3) * layout.cellWidth;
                      x++
                    ) {
                      if (before.data[(y * before.width + x) * 4] > 100) {
                        left = Math.min(left, x);
                        right = Math.max(right, x);
                      }
                    }
                  }
                  expect(
                    Math.abs(left - (column + 0.45) * layout.cellWidth)
                  ).toBeLessThanOrEqual(2);
                  expect(
                    Math.abs(right + 1 - (column + 2.55) * layout.cellWidth)
                  ).toBeLessThanOrEqual(2);
                }
              }
              await app.input.pressKey('c');
              await readReport(1);
              await waitForResult(async () => {
                const after = await capture();
                let changed = 0;
                for (let row = 0; row < samples.length; row++) {
                  // The recolored glyph may touch the first pixel of the next cell.
                  const following =
                    (samples[row].indexOf('①') + width) * layout.cellWidth + 1;
                  for (
                    let y = row * layout.cellHeight;
                    y < (row + 1) * layout.cellHeight;
                    y++
                  ) {
                    for (
                      let x = 0;
                      x < (expectedColumns[row] - 1) * layout.cellWidth;
                      x++
                    ) {
                      const offset = (y * after.width + x) * 4;
                      const equal = before.data
                        .subarray(offset, offset + 3)
                        .equals(after.data.subarray(offset, offset + 3));
                      if (x >= following)
                        expect(
                          equal,
                          `following glyph at ${row}:${x},${y}`
                        ).toBe(true);
                      else if (!equal) changed++;
                    }
                  }
                }
                expect(changed).toBeGreaterThan(0);
              });
              await evidence.captureEvidence(
                `${active}-cell-placement`,
                async () => terminal.capture()
              );

              // Cross the drag threshold before returning to the digit's end.
              await app.input.setModifier('control', true);
              await app.input.moveMouseTo(
                layout.terminalCapture.bounds.x + 1,
                layout.terminalCapture.bounds.y +
                  Math.floor(layout.cellHeight / 2)
              );
              await app.input.setMouseButton('left', true);
              await app.input.moveMouseTo(
                layout.terminalCapture.bounds.x +
                  (width + 2) * layout.cellWidth,
                layout.terminalCapture.bounds.y +
                  Math.floor(layout.cellHeight / 2)
              );
              await app.input.moveMouseTo(
                layout.terminalCapture.bounds.x + width * layout.cellWidth - 1,
                layout.terminalCapture.bounds.y +
                  Math.floor(layout.cellHeight / 2)
              );
              await app.input.setMouseButton('left', false);
              await app.input.setModifier('control', false);
              await waitForResult(async () =>
                expect(await state.text()).toContain(
                  'selected=1 selection_rows=1'
                )
              );
              await pressKeyWithModifiers(app, ['control', 'shift'], 'c');
              await waitForResult(async () => {
                const copied = await exec(clipboardReader, [], {
                  env: await app.environment(),
                });
                expect(copied.stdout).toBe('①');
              });
              await app.input.pressKey('Escape');

              await expectElementKind(
                await app.getById('application_menu_button'),
                'toggleButton'
              ).click();
              await expectElementKind(
                await app.getById('settings_menu_item'),
                'menuItem'
              ).click();
              const notebook = expectElementKind(
                await app.getById('settings_notebook'),
                'tabList'
              );
              for (let i = 0; i < (await notebook.getChildCount()); i++) {
                if (
                  (await (await notebook.childAt(i))?.info())?.name ===
                  'Terminal'
                ) {
                  await notebook.selectChildAt(i);
                  break;
                }
              }
              const combo = expectElementKind(
                await app.getById('settings_terminal_ambiguous_width_combo'),
                'comboBox'
              );
              const dialog = await app.getById('settings_dialog');
              expect(
                (await (await combo.selectedChildAt(0))?.info())?.name
              ).toContain(
                width === 1
                  ? '1 column'
                  : width === 2
                    ? '2 columns'
                    : 'Font dependent'
              );
              await combo.selectChildAt(
                reloaded ? 0 : saved === 'narrow' ? 1 : saved === 'wide' ? 2 : 3
              );
              await expectElementKind(
                await app.getById(
                  reloaded ? 'settings_apply_button' : 'settings_save_button'
                ),
                'button'
              ).click();
              await waitForResult(async () => {
                let showing = false;
                try {
                  showing = (await dialog.info()).states.includes('showing');
                } catch {
                  // Closing the dialog can invalidate its accessibility object.
                }
                expect(showing).toBe(false);
              });
              await waitForResult(async () =>
                expect(await state.text()).toContain('ready=1')
              );
              await focus();
              await waitForResult(async () =>
                expect((await terminal.info()).states).toContain('focused')
              );
              await app.input.pressKey('r');
              const retained = await readReport(2);
              expect(retained.pid).toBe(started.pid);
              expect(await readFile(path, 'utf8')).toContain(
                `ambiguous_width=${saved}`
              );
            },
            {
              globalSettings: `[terminal]\n${initial === 'font' ? '' : `ambiguous_width=${initial}\n`}font_families=Boxrig Width ASCII;Boxrig Width Fixture;\nfont_size_dip=20\n`,
              env: {
                GTK_IM_MODULE: 'gtk-im-context-simple',
                ...(backend === 'cairo'
                  ? { GDK_GL: 'disable' }
                  : { LIBGL_ALWAYS_SOFTWARE: '1' }),
              },
            }
          );
        }
      });
    }, 120_000);
  }
}
