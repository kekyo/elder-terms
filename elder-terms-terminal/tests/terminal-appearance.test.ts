// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { chmod, readFile, writeFile } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  runGtkTest,
  withTemporaryDirectory,
  readTerminalGridLayout,
} from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const exec = promisify(execFile);
const luminance = (rgb: number): number => {
  const linear = (byte: number): number => {
    const channel = byte / 255;
    return channel <= 0.04045
      ? channel / 12.92
      : ((channel + 0.055) / 1.055) ** 2.4;
  };
  return (
    0.2126 * linear((rgb >>> 16) & 255) +
    0.7152 * linear((rgb >>> 8) & 255) +
    0.0722 * linear(rgb & 255)
  );
};

describe('terminal appearance', () => {
  for (const [shape, backend] of ['block', 'bar', 'underline'].flatMap(
    (shape) => ['opengl', 'cairo'].map((backend) => [shape, backend])
  )) {
    it(`applies the ${shape} cursor, outlines it without focus and records independent blinking (${backend})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const config = join(directory, 'cursor.ini');
        const peer = join(directory, 'cursor.mjs');
        await writeFile(
          peer,
          `process.stdin.setRawMode(true); process.stdin.resume();
process.stdout.write('\\x1b]12;#ffffff\\x07\\x1b[48;2;0;0;0m\\x1b[2J\\x1b[H\\x1b[?25h\\x1b[6 q');
`
        );
        await writeFile(
          config,
          `[terminal]\ncursor_shape=${shape}\ncursor_blink=false\n[local]\ncommand_line=${process.execPath} ${peer}\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const layout = await readTerminalGridLayout(app);
            const terminal = await app.getById('terminal_view');
            // The existing modal overlay dims white to 166 while Settings is open.
            // Count the box geometry in both states, preserving that behavior.
            const count = async (): Promise<number> => {
              const png = PNG.sync.read((await terminal.capture()).image);
              let ink = 0;
              for (let y = 0; y < layout.cellHeight; y++)
                for (let x = 0; x < layout.cellWidth; x++) {
                  const offset = (y * png.width + x) * 4;
                  if (
                    png.data[offset] > 100 &&
                    png.data[offset + 1] > 100 &&
                    png.data[offset + 2] > 100
                  )
                    ink++;
                }
              return ink;
            };
            const width =
              shape === 'bar'
                ? Math.min(4, layout.cellWidth)
                : layout.cellWidth;
            const height =
              shape === 'underline'
                ? Math.min(4, layout.cellHeight)
                : layout.cellHeight;
            const filled = width * height;
            const outlined =
              filled - Math.max(0, width - 2) * Math.max(0, height - 2);
            const record = async (name: string): Promise<number[]> => {
              const env = await app.environment();
              const video = join(evidence.directory, `${name}.mkv`);
              // Record the cell itself at its screen coordinates; assert decoded
              // pixels over multiple complete blink periods instead of timed sleeps.
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
                  `${layout.cellWidth}x${layout.cellHeight}`,
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
                { encoding: 'buffer', maxBuffer: 1024 * 1024 }
              );
              const stride = layout.cellWidth * layout.cellHeight * 3;
              const counts: number[] = [];
              for (let start = 0; start < raw.stdout.length; start += stride) {
                let ink = 0;
                for (let offset = start; offset < start + stride; offset += 3)
                  if (
                    raw.stdout[offset] > 100 &&
                    raw.stdout[offset + 1] > 100 &&
                    raw.stdout[offset + 2] > 100
                  )
                    ink++;
                counts.push(ink);
              }
              expect(counts).toHaveLength(90);
              await writeFile(
                join(evidence.directory, name + '-pixels.json'),
                JSON.stringify(counts)
              );
              return counts;
            };
            await app.input.moveMouseTo(
              layout.terminalCapture.bounds.x + 50,
              layout.terminalCapture.bounds.y + 50
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await waitForResult(async () => expect(await count()).toBe(filled));
            await evidence.captureEvidence('focused-box', async () =>
              terminal.capture()
            );
            expect(
              (await record('cursor-steady')).every((value) => value === filled)
            ).toBe(true);
            const openSettings = async (): Promise<void> => {
              await expectElementKind(
                await app.getById('application_menu_button'),
                'toggleButton'
              ).click();
              await expectElementKind(
                await app.getById('settings_menu_item'),
                'menuItem'
              ).click();
              await expectElementKind(
                await app.getById('settings_dialog'),
                'window'
              ).moveTo(500, 100);
            };
            await openSettings();
            await waitForResult(async () =>
              expect(await count()).toBe(outlined)
            );
            await evidence.captureEvidence('unfocused-box', async () =>
              terminal.capture()
            );
            expect(
              (await record('cursor-unfocused')).every(
                (value) => value === outlined
              )
            ).toBe(true);
            const notebook = expectElementKind(
              await app.getById('settings_notebook'),
              'tabList'
            );
            for (let i = 0; i < (await notebook.getChildCount()); i++) {
              if (
                (await (await notebook.childAt(i))?.info())?.name ===
                'Terminal appearance'
              ) {
                await notebook.selectChildAt(i);
                break;
              }
            }
            const shapeCombo = expectElementKind(
              await app.getById('settings_terminal_cursor_shape_combo'),
              'comboBox'
            );
            expect(
              (await (await shapeCombo.selectedChildAt(0))?.info())?.name
            ).toBe(
              shape === 'block'
                ? 'Full box'
                : shape === 'bar'
                  ? 'Vertical box'
                  : 'Underline box'
            );
            await expectElementKind(
              await app.getById('settings_terminal_cursor_blink_combo'),
              'comboBox'
            ).selectChildAt(1);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(
                (await (await app.getById('settings_dialog')).info()).states
              ).toContain('showing');
            });
            await expectElementKind(
              await app.getById('settings_cancel_button'),
              'button'
            ).click();
            expect(await readFile(config, 'utf8')).toContain(
              'cursor_blink=false'
            );
            await app.input.moveMouseTo(
              layout.terminalCapture.bounds.x + 50,
              layout.terminalCapture.bounds.y + 50
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await waitForResult(async () => expect(await count()).toBe(filled));
            const blinking = await record('cursor-blink');
            expect(
              blinking.filter((value) => value === filled).length
            ).toBeGreaterThan(20);
            expect(
              blinking.filter((value) => value === 0).length
            ).toBeGreaterThan(20);
            expect(
              blinking.every((value) => value === 0 || value === filled)
            ).toBe(true);
            await openSettings();
            const next =
              shape === 'block'
                ? 'bar'
                : shape === 'bar'
                  ? 'underline'
                  : 'block';
            const nextIndex = ['block', 'bar', 'underline'].indexOf(next) + 1;
            await expectElementKind(
              await app.getById('settings_terminal_cursor_shape_combo'),
              'comboBox'
            ).selectChildAt(nextIndex);
            await expectElementKind(
              await app.getById('settings_terminal_cursor_blink_combo'),
              'comboBox'
            ).selectChildAt(2);
            await expectElementKind(
              await app.getById('settings_save_button'),
              'button'
            ).click();
            await waitForResult(async () =>
              expect(await readFile(config, 'utf8')).toContain(
                `cursor_shape=${next}`
              )
            );
            expect(await readFile(config, 'utf8')).toContain(
              'cursor_blink=false'
            );
            await app.input.moveMouseTo(
              layout.terminalCapture.bounds.x + 50,
              layout.terminalCapture.bounds.y + 50
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            const nextFilled =
              (next === 'bar' ? 4 : layout.cellWidth) *
              (next === 'underline' ? 4 : layout.cellHeight);
            await waitForResult(async () =>
              expect(await count()).toBe(nextFilled)
            );
          },
          {
            env: {
              LIBGL_ALWAYS_SOFTWARE: '1',
              ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
            },
          }
        );
      });
    });
  }

  it('keeps the active renderer at the left and actions at the right of the settings footer', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const config = join(directory, 'renderer.ini');
      await writeFile(config, '[terminal]\n');
      await runGtkTest(
        context,
        ['--test-fixture', '-c', config],
        async (app, evidence) => {
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          const label = expectElementKind(
            await app.getById('settings_runtime_status'),
            'label'
          );
          await waitForResult(async () =>
            expect(await label.text()).toMatch(/^(OpenGL( ES)?|Cairo) \/ X11$/)
          );
          const native = await expectElementKind(
            await app.getById('fixture_terminal_state_label'),
            'label'
          ).text();
          expect(
            (await label.text()).startsWith(
              native.includes('backend=opengl') ? 'OpenGL' : 'Cairo'
            )
          ).toBe(true);
          for (const width of [560, 900]) {
            await expectElementKind(
              await app.getById('settings_dialog'),
              'window'
            ).resizeTo(width, 430);
            await waitForResult(async () => {
              const panel = (
                await (await app.getById('settings_action_row')).capture()
              ).bounds;
              const status = (await label.capture()).bounds;
              const apply = (
                await (await app.getById('settings_apply_button')).capture()
              ).bounds;
              const save = (
                await (await app.getById('settings_save_button')).capture()
              ).bounds;
              const cancel = (
                await (await app.getById('settings_cancel_button')).capture()
              ).bounds;
              expect(status.x - panel.x).toBe(12);
              expect(status.x + status.width).toBeLessThanOrEqual(apply.x);
              expect(apply.x + apply.width).toBeLessThan(save.x);
              expect(save.x + save.width).toBeLessThan(cancel.x);
              expect(panel.x + panel.width - cancel.x - cancel.width).toBe(12);
              expect(
                Math.abs(
                  status.y + status.height / 2 - cancel.y - cancel.height / 2
                )
              ).toBeLessThan(2);
            });
          }
          await evidence.captureEvidence('renderer-footer', async () =>
            app.capture()
          );
          expect(
            (await (await app.getById('settings_save_button')).info()).states
          ).toContain('showing');
        }
      );
    });
  });

  for (const background of ['none', '#101010', '#efefef', '#613583']) {
    it(`keeps the scrollbar visible and usable on ${background}`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const config = join(directory, 'terminal.ini');
        const shell = join(directory, 'shell.sh');
        const release = join(directory, 'release');
        await exec('mkfifo', [release]);
        await writeFile(
          shell,
          `#!/bin/sh\ni=0\nwhile [ "$i" -lt 100 ]; do printf 'line %s\\r\\n' "$i"; i=$((i+1)); done\nread -r done < '${release}'\n`
        );
        await chmod(shell, 0o755);
        await writeFile(
          config,
          `[general]\nbackground=${background}\n[terminal]\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const bar = expectElementKind(
              await app.getById('terminal_scrollbar'),
              'scrollbar'
            );
            await waitForResult(async () =>
              expect(await bar.value()).toBeGreaterThan(50)
            );
            const before = await waitForResult(async () =>
              readTerminalGridLayout(app)
            );
            const capture = await waitForResult(async () => {
              const shot = await bar.capture();
              const png = PNG.sync.read(shot.image);
              const colors = new Map<number, number>();
              for (let y = 10; y < png.height - 10; y++) {
                const at = (y * png.width + Math.floor(png.width / 2)) * 4;
                const rgb =
                  png.data[at] * 65536 +
                  png.data[at + 1] * 256 +
                  png.data[at + 2];
                colors.set(rgb, (colors.get(rgb) ?? 0) + 1);
              }
              const dominant = [...colors]
                .sort((a, b) => b[1] - a[1])
                .slice(0, 2);
              expect(dominant).toHaveLength(2);
              expect(dominant[1][1]).toBeGreaterThan(5);
              const levels = dominant
                .map(([rgb]) => luminance(rgb))
                .sort((a, b) => a - b);
              expect(
                (levels[1] + 0.05) / (levels[0] + 0.05)
              ).toBeGreaterThanOrEqual(3);
              return shot;
            });
            await evidence.captureEvidence('scrollbar-visible', async () =>
              app.capture()
            );
            const initial = await bar.value();
            await app.input.moveMouseTo(
              Math.round(before.terminalCapture.bounds.x + 40),
              Math.round(before.terminalCapture.bounds.y + 40)
            );
            await app.input.scrollWheel(0, -5);
            await waitForResult(async () =>
              expect(await bar.value()).toBeLessThan(initial)
            );
            await bar.setValue(initial);
            const bounds = capture.bounds;
            const x = Math.round(bounds.x + bounds.width / 2);
            await app.input.moveMouseTo(
              x,
              Math.round(bounds.y + bounds.height * 0.9)
            );
            await app.input.setMouseButton('left', true);
            await app.input.moveMouseTo(
              x,
              Math.round(bounds.y + bounds.height * 0.3)
            );
            await app.input.setMouseButton('left', false);
            await waitForResult(async () =>
              expect(await bar.value()).toBeLessThan(initial)
            );
            const after = await readTerminalGridLayout(app);
            expect(after.terminalCapture.bounds).toEqual(
              before.terminalCapture.bounds
            );
          },
          { env: { SHELL: shell } }
        );
      });
    });
  }
});
